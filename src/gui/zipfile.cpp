
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/types.h>
#include <algorithm> // std::transform
#ifdef WIN32
# include <signal.h>
# include <sys/stat.h>
# include <process.h>
#else
# include <fcntl.h>
#endif

#include "dosbox.h"
#include "pic.h"
#include "timer.h"
#include "setup.h"
#include "bios.h"
#include "support.h"
#include "debug.h"
#include "ide.h"
#include "bitop.h"
#include "ptrop.h"
#include "mapper.h"
#include "zipfile.h"

#include "mapper.h"
#include "vga.h"
#include "keyboard.h"
#include "cpu.h"
#include "fpu.h"
#include "cross.h"
#include "keymap.h"

off_t ZIPFileEntry::seek_file(off_t pos) {
    if (file == NULL || file_offset == (off_t)0) return (off_t)(-1LL);

    /* no seeking while writing, the CRC generation depends on a streaming write */
    if (pos != position && can_extend) return (off_t)(-1LL);

    if (pos < (off_t)0) pos = (off_t)0;
    if (pos > (off_t)file_length) pos = (off_t)file_length;
    pos = file->seek_file(pos + file_offset) - file_offset;
    if (pos < 0 || pos > (off_t)file_length) return (off_t)(-1LL);
    position = pos;
    return pos;
}

ssize_t ZIPFileEntry::read(void *buffer,size_t count) {
    if (file == NULL || file_offset == (off_t)0) return -1;
    if (position >= file_length) return 0;

    size_t mread = file_length - position;
    if (mread > count) mread = count;

    if (mread > 0) {
        if (seek_file(position) != position) return -1;
        mread = file->read(buffer,mread);
        if (mread > 0) position += mread;
    }

    return mread;
}

ssize_t ZIPFileEntry::write(const void *buffer,size_t count) {
    if (file == NULL || file_offset == (off_t)0 || !can_write) return -1;
    if (position > file_length) return 0;
    if (position == file_length && !can_extend) return 0;

    size_t mwrite;

    if (can_extend) {
        mwrite = count;
    }
    else {
        mwrite = file_length - position;
        if (mwrite > count) mwrite = count;
    }

    if (mwrite > 0) {
        if (seek_file(position) != position) return -1;
        mwrite = file->write(buffer,mwrite);
        if (mwrite > 0) {
            position += mwrite;
            write_crc = zipcrc_update(write_crc, buffer, count);
            if (file_length < position && can_extend) file_length = position;
        }
    }

    return mwrite;
}

ZIPFile::ZIPFile() {
}

ZIPFile::~ZIPFile() {
    close();
}

void ZIPFile::close(void) {
    if (file_fd >= 0) {
        ::close(file_fd);
        file_fd = -1;
    }

    entries.clear();
}

ZIPFileEntry *ZIPFile::get_entry(const char *name) {
    if (file_fd < 0) return NULL;

    if (*name == 0) return NULL;

    auto i = entries.find(name);
    if (i == entries.end()) return NULL;

    return &(i->second);
}

ZIPFileEntry *ZIPFile::new_entry(const char *name) {
    if (file_fd < 0 || !can_write) return NULL;

    auto i = entries.find(name);
    if (i != entries.end()) return NULL;

    if (*name == 0) return NULL;

    close_current();
    current_entry = name;
    write_pos = end_of_file();

    ZIPFileEntry *ent = &entries[name];
    ent->name = name;
    ent->can_write = true;
    ent->can_extend = true;
    ent->file_header_offset = write_pos;
    write_pos += sizeof(ZIPLocalFileHeader) + ent->name.length();
    ent->write_crc = zipcrc_init();
    ent->file_offset = write_pos;
    ent->file = this;

    if (seek_file(ent->file_header_offset) != ent->file_header_offset) {
        close_current();
        return NULL;
    }

    ZIPLocalFileHeader hdr;
    memset(&hdr,0,sizeof(hdr));
    hdr.local_file_header_signature = htole32(0x04034b50);  /* PK\x03\x04 */
    hdr.version_needed_to_extract = htole16(20);            /* PKZIP 2.0 */
    hdr.general_purpose_bit_flag = htole16(0 << 1);
    hdr.compression_method = 0;                             /* store (no compression) */
    hdr.file_name_length = htole16(ent->name.length());
    if (write(&hdr,sizeof(hdr)) != sizeof(hdr)) {
        close_current();
        return NULL;
    }
    assert(ent->name.length() != 0);
    if ((size_t)write(ent->name.c_str(),ent->name.length()) != ent->name.length()) {
        close_current();
        return NULL;
    }
    if (seek_file(ent->file_offset) != ent->file_offset) {
        close_current();
        return NULL;
    }

    return ent;
}

off_t ZIPFile::end_of_file(void) {
    return lseek(file_fd,0,SEEK_END);
}

void ZIPFile::close_current(void) {
    if (!can_write) return;

    if (!current_entry.empty()) {
        ZIPFileEntry *ent = get_entry(current_entry.c_str());
        ZIPLocalFileHeader hdr;

        if (ent != NULL && ent->can_write) {
            ent->can_write = false;
            ent->can_extend = false;

            if (seek_file(ent->file_header_offset) == ent->file_header_offset && read(&hdr,sizeof(hdr)) == sizeof(hdr)) {
                hdr.compressed_size = hdr.uncompressed_size = htole32(ent->file_length);
                hdr.crc_32 = htole32(zipcrc_finalize(ent->write_crc));

                if (seek_file(ent->file_header_offset) == ent->file_header_offset && write(&hdr,sizeof(hdr)) == sizeof(hdr)) {
                    /* good */
                }
            }
        }
    }

    current_entry.clear();
}

int ZIPFile::open(const char *path,int mode) {
    unsigned char tmp[512];

    close();

    if (path == NULL) return -1;

    if ((mode & 3) == O_WRONLY) {
        LOG_MSG("WARNING: ZIPFile attempt to open with O_WRONLY, which will not work");
        return -1;
    }

    file_fd = ::open(path,mode,0644);
    if (file_fd < 0) return -1;
    if (lseek(file_fd,0,SEEK_SET) != 0) {
        close();
        return -1;
    }

    entries.clear();
    current_entry.clear();
    wrote_trailer = false;
    write_pos = 0;

    /* WARNING: This assumes O_RDONLY, O_WRONLY, O_RDWR are defined as in Linux (0, 1, 2) in the low two bits */
    if ((mode & 3) == O_RDWR)
        can_write = true;
    else
        can_write = false;

    /* if we're supposed to READ the ZIP file, then start scanning now */
    if ((mode & 3) == O_RDONLY) {
        struct pkzip_central_directory_header_main chdr;
        struct pkzip_central_directory_header_end ehdr;

        off_t fsz = end_of_file();

        /* check for 'PK' at the start of the file.
         * This code only expects to handle the ZIP files it generated, not ZIP files in general. */
        if (fsz < 64 || seek_file(0) != 0 || read(tmp,4) != 4 || memcmp(tmp,"PK\x03\x04",4) != 0) {
            LOG_MSG("Not a PKZIP file");
            close();
            return -1;
        }

        /* then look for the central directory at the end.
         * this code DOES NOT SUPPORT the ZIP comment field, nor will this code generate one. */
        if (seek_file(fsz - (off_t)sizeof(ehdr)) != (fsz - (off_t)sizeof(ehdr)) || (size_t)read(&ehdr,sizeof(ehdr)) != sizeof(ehdr) || ehdr.sig != PKZIP_CENTRAL_DIRECTORY_END_SIG || ehdr.size_of_central_directory > 0x100000/*absurd size*/ || ehdr.offset_of_central_directory_from_start_disk == 0 || ehdr.offset_of_central_directory_from_start_disk >= fsz) {
            LOG_MSG("Cannot locate Central Directory");
            close();
            return -1;
        }
        if (seek_file(ehdr.offset_of_central_directory_from_start_disk) != ehdr.offset_of_central_directory_from_start_disk) {
            LOG_MSG("Cannot locate Central Directory #2");
            close();
            return -1;
        }

        /* read the central directory */
        {
            long remain = (long)ehdr.size_of_central_directory;

            while (remain >= (long)sizeof(struct pkzip_central_directory_header_main)) {
                if (read(&chdr,sizeof(chdr)) != sizeof(chdr)) break;
                remain -= sizeof(chdr);

                if (chdr.sig != PKZIP_CENTRAL_DIRECTORY_HEADER_SIG) break;
                if (chdr.filename_length >= sizeof(tmp)) break;

                tmp[chdr.filename_length] = 0;
                if (chdr.filename_length != 0) {
                    if (read(tmp,chdr.filename_length) != chdr.filename_length) break;
                    remain -= chdr.filename_length;
                }

                if (tmp[0] == 0) continue;

                ZIPFileEntry *ent = &entries[(char*)tmp];
                ent->can_write = false;
                ent->can_extend = false;
                ent->file_length = htole32(chdr.uncompressed_size);
                ent->file_header_offset = htole32(chdr.relative_offset_of_local_header);
                ent->file_offset = ent->file_header_offset + sizeof(struct ZIPLocalFileHeader) + htole16(chdr.filename_length) + htole16(chdr.extra_field_length);
                ent->position = 0;
                ent->name = (char*)tmp;
                ent->file = this;
            }
        }
    }

    return 0;
}

off_t ZIPFile::seek_file(off_t pos) {
    if (file_fd < 0) return (off_t)(-1LL);
    return ::lseek(file_fd,pos,SEEK_SET);
}

ssize_t ZIPFile::read(void *buffer,size_t count) {
    if (file_fd < 0) return -1;
    return ::read(file_fd,buffer,count);
}

ssize_t ZIPFile::write(const void *buffer,size_t count) {
    if (file_fd < 0) return -1;
    return ::write(file_fd,buffer,count);
}

void ZIPFile::writeZIPFooter(void) {
    struct pkzip_central_directory_header_main chdr;
    struct pkzip_central_directory_header_end ehdr;
    uint32_t cdircount = 0;
    uint32_t cdirbytes = 0;
    off_t cdirofs = 0;

    if (file_fd < 0 || wrote_trailer || !can_write) return;

    close_current();
    cdirofs = end_of_file();

    for (auto i=entries.begin();i!=entries.end();i++) {
        const ZIPFileEntry &ent = i->second;

        memset(&chdr,0,sizeof(chdr));
        chdr.sig = htole32(PKZIP_CENTRAL_DIRECTORY_HEADER_SIG);
        chdr.version_made_by = htole16((0 << 8) + 20);      /* PKZIP 2.0 */
        chdr.version_needed_to_extract = htole16(20);       /* PKZIP 2.0 or higher */
        chdr.general_purpose_bit_flag = htole16(0 << 1);    /* just lie and say that "normal" deflate was used */
        chdr.compression_method = 0;                        /* stored (no compression) */
        chdr.last_mod_file_time = 0;
        chdr.last_mod_file_date = 0;
        chdr.compressed_size = htole32(ent.file_length);
        chdr.uncompressed_size = htole32(ent.file_length);
        chdr.filename_length = htole16(ent.name.length());
        chdr.disk_number_start = htole16(1);
        chdr.internal_file_attributes = 0;
        chdr.external_file_attributes = 0;
        chdr.relative_offset_of_local_header = htole32(ent.file_header_offset);
        chdr.crc32 = htole32(zipcrc_finalize(ent.write_crc));

        if (write(&chdr,sizeof(chdr)) != sizeof(chdr)) break;
        cdirbytes += sizeof(chdr);
        cdircount++;

        assert(ent.name.length() != 0);
        if ((size_t)write(ent.name.c_str(),ent.name.length()) != ent.name.length()) break;
        cdirbytes += ent.name.length();
    }

    memset(&ehdr,0,sizeof(ehdr));
    ehdr.sig = htole32(PKZIP_CENTRAL_DIRECTORY_END_SIG);
    ehdr.number_of_disk_with_start_of_central_directory = htole16(0);
    ehdr.number_of_this_disk = htole16(0);
    ehdr.total_number_of_entries_of_central_dir_on_this_disk = htole16(cdircount);
    ehdr.total_number_of_entries_of_central_dir = htole16(cdircount);
    ehdr.size_of_central_directory = htole32(cdirbytes);
    ehdr.offset_of_central_directory_from_start_disk = htole32(cdirofs);
    write(&ehdr,sizeof(ehdr));

    wrote_trailer = true;
    current_entry.clear();
}

