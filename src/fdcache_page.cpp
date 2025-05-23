/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Takeshi Nakatani <ggtakec.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <cstdio>
#include <cerrno>
#include <memory>
#include <unistd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "common.h"
#include "s3fs_logger.h"
#include "fdcache_page.h"
#include "fdcache_stat.h"
#include "string_util.h"

//------------------------------------------------
// Symbols
//------------------------------------------------
static constexpr int CHECK_CACHEFILE_PART_SIZE = 1024 * 16;    // Buffer size in PageList::CheckZeroAreaInFile()

//------------------------------------------------
// fdpage_list_t utility
//------------------------------------------------
// Inline function for repeated processing
static inline void raw_add_compress_fdpage_list(fdpage_list_t& pagelist, const fdpage& orgpage, bool ignore_load, bool ignore_modify, bool default_load, bool default_modify)
{
    if(0 < orgpage.bytes){
        // [NOTE]
        // The page variable is subject to change here.
        //
        fdpage page = orgpage;

        if(ignore_load){
            page.loaded   = default_load;
        }
        if(ignore_modify){
            page.modified = default_modify;
        }
        pagelist.push_back(page);
    }
}

// Compress the page list
//
// ignore_load:     Ignore the flag of loaded member and compress
// ignore_modify:   Ignore the flag of modified member and compress
// default_load:    loaded flag value in the list after compression when ignore_load=true
// default_modify:  modified flag value in the list after compression when default_modify=true
//
// NOTE: ignore_modify and ignore_load cannot both be true.
//       Zero size pages will be deleted. However, if the page information is the only one,
//       it will be left behind. This is what you need to do to create a new empty file.
//
static void raw_compress_fdpage_list(const fdpage_list_t& pages, fdpage_list_t& compressed_pages, bool ignore_load, bool ignore_modify, bool default_load, bool default_modify)
{
    compressed_pages.clear();

    fdpage*                 lastpage = nullptr;
    fdpage_list_t::iterator add_iter;
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(0 == iter->bytes){
            continue;
        }
        if(!lastpage){
            // First item
            raw_add_compress_fdpage_list(compressed_pages, (*iter), ignore_load, ignore_modify, default_load, default_modify);
            lastpage = &(compressed_pages.back());
        }else{
            // check page continuity
            if(lastpage->next() != iter->offset){
                // Non-consecutive with last page, so add a page filled with default values
                if( (!ignore_load   && (lastpage->loaded   != false)) ||
                    (!ignore_modify && (lastpage->modified != false)) )
                {
                    // add new page
                    fdpage tmppage(lastpage->next(), (iter->offset - lastpage->next()), false, false);
                    raw_add_compress_fdpage_list(compressed_pages, tmppage, ignore_load, ignore_modify, default_load, default_modify);

                    add_iter = compressed_pages.end();
                    --add_iter;
                    lastpage = &(*add_iter);
                }else{
                    // Expand last area
                    lastpage->bytes = iter->offset - lastpage->offset;
                }
            }

            // add current page
            if( (!ignore_load   && (lastpage->loaded   != iter->loaded  )) ||
                (!ignore_modify && (lastpage->modified != iter->modified)) )
            {
                // Add new page
                raw_add_compress_fdpage_list(compressed_pages, (*iter), ignore_load, ignore_modify, default_load, default_modify);

                add_iter = compressed_pages.end();
                --add_iter;
                lastpage = &(*add_iter);
            }else{
                // Expand last area
                lastpage->bytes += iter->bytes;
            }
        }
    }
}

static void compress_fdpage_list_ignore_modify(const fdpage_list_t& pages, fdpage_list_t& compressed_pages, bool default_modify)
{
    raw_compress_fdpage_list(pages, compressed_pages, /* ignore_load= */ false, /* ignore_modify= */ true, /* default_load= */false, /* default_modify= */default_modify);
}

static void compress_fdpage_list_ignore_load(const fdpage_list_t& pages, fdpage_list_t& compressed_pages, bool default_load)
{
    raw_compress_fdpage_list(pages, compressed_pages, /* ignore_load= */ true, /* ignore_modify= */ false, /* default_load= */default_load, /* default_modify= */false);
}

static void compress_fdpage_list(const fdpage_list_t& pages, fdpage_list_t& compressed_pages)
{
    raw_compress_fdpage_list(pages, compressed_pages, /* ignore_load= */ false, /* ignore_modify= */ false, /* default_load= */false, /* default_modify= */false);
}

static fdpage_list_t parse_partsize_fdpage_list(const fdpage_list_t& pages, off_t max_partsize)
{
    fdpage_list_t parsed_pages;
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->modified){
            // modified page
            fdpage tmppage = *iter;
            for(off_t start = iter->offset, rest_bytes = iter->bytes; 0 < rest_bytes; ){
                if((max_partsize * 2) < rest_bytes){
                    // do parse
                    tmppage.offset = start;
                    tmppage.bytes  = max_partsize;
                    parsed_pages.push_back(tmppage);

                    start      += max_partsize;
                    rest_bytes -= max_partsize;
                }else{
                    // Since the number of remaining bytes is less than twice max_partsize,
                    // one of the divided areas will be smaller than max_partsize.
                    // Therefore, this area at the end should not be divided.
                    tmppage.offset = start;
                    tmppage.bytes  = rest_bytes;
                    parsed_pages.push_back(tmppage);

                    start      += rest_bytes;
                    rest_bytes  = 0;
                }
            }
        }else{
            // not modified page is not parsed
            parsed_pages.push_back(*iter);
        }
    }
    return parsed_pages;
}

//------------------------------------------------
// PageList class methods
//------------------------------------------------
//
// Examine and return the status of each block in the file.
//
// Assuming the file is a sparse file, check the HOLE and DATA areas
// and return it in fdpage_list_t. The loaded flag of each fdpage is
// set to false for HOLE blocks and true for DATA blocks.
//
bool PageList::GetSparseFilePages(int fd, size_t file_size, fdpage_list_t& sparse_list)
{
    // [NOTE]
    // Express the status of the cache file using fdpage_list_t.
    // There is a hole in the cache file(sparse file), and the
    // state of this hole is expressed by the "loaded" member of
    // struct fdpage. (the "modified" member is not used)
    //
    if(0 == file_size){
        // file is empty
        return true;
    }

    bool is_hole   = false;
    off_t hole_pos = lseek(fd, 0, SEEK_HOLE);
    off_t data_pos = lseek(fd, 0, SEEK_DATA);
    if(-1 == hole_pos && -1 == data_pos){
        S3FS_PRN_ERR("Could not find the first position both HOLE and DATA in the file(physical_fd=%d).", fd);
        return false;
    }else if(-1 == hole_pos){
        is_hole   = false;
    }else if(-1 == data_pos){
        is_hole   = true;
    }else if(hole_pos < data_pos){
        is_hole   = true;
    }else{
        is_hole   = false;
    }

    for(off_t cur_pos = 0, next_pos = 0; 0 <= cur_pos; cur_pos = next_pos, is_hole = !is_hole){
        fdpage page;
        page.offset   = cur_pos;
        page.loaded   = !is_hole;
        page.modified = false;

        next_pos = lseek(fd, cur_pos, (is_hole ? SEEK_DATA : SEEK_HOLE));
        if(-1 == next_pos){
            page.bytes = static_cast<off_t>(file_size - cur_pos);
        }else{
            page.bytes = next_pos - cur_pos;
        }
        sparse_list.push_back(page);
    }
    return true;
}

//
// Confirm that the specified area is ZERO
//
bool PageList::CheckZeroAreaInFile(int fd, off_t start, size_t bytes)
{
    auto readbuff = std::make_unique<char[]>(CHECK_CACHEFILE_PART_SIZE);

    for(size_t comp_bytes = 0, check_bytes = 0; comp_bytes < bytes; comp_bytes += check_bytes){
        if(CHECK_CACHEFILE_PART_SIZE < (bytes - comp_bytes)){
            check_bytes = CHECK_CACHEFILE_PART_SIZE;
        }else{
            check_bytes = bytes - comp_bytes;
        }
        bool    found_bad_data = false;
        ssize_t read_bytes;
        if(-1 == (read_bytes = pread(fd, readbuff.get(), check_bytes, (start + comp_bytes)))){
            S3FS_PRN_ERR("Something error is occurred in reading %zu bytes at %lld from file(physical_fd=%d).", check_bytes, static_cast<long long int>(start + comp_bytes), fd);
            found_bad_data = true;
        }else{
            check_bytes = static_cast<size_t>(read_bytes);
            for(size_t tmppos = 0; tmppos < check_bytes; ++tmppos){
                if('\0' != readbuff[tmppos]){
                    // found not ZERO data.
                    found_bad_data = true;
                    break;
                }
            }
        }
        if(found_bad_data){
            return false;
        }
    }
    return true;
}

//
// Checks that the specified area matches the state of the sparse file.
//
// [Parameters]
// checkpage:    This is one state of the cache file, it is loaded from the stats file.
// sparse_list:  This is a list of the results of directly checking the cache file status(HOLE/DATA).
//               In the HOLE area, the "loaded" flag of fdpage is false. The DATA area has it set to true.
// fd:           opened file descriptor to target cache file.
//
bool PageList::CheckAreaInSparseFile(const struct fdpage& checkpage, const fdpage_list_t& sparse_list, int fd, fdpage_list_t& err_area_list, fdpage_list_t& warn_area_list)
{
    // Check the block status of a part(Check Area: checkpage) of the target file.
    // The elements of sparse_list have 5 patterns that overlap this block area.
    //
    // File           |<---...--------------------------------------...--->|
    // Check Area              (offset)<-------------------->(offset + bytes - 1)
    // Area case(0)       <------->
    // Area case(1)                                            <------->
    // Area case(2)              <-------->
    // Area case(3)                                 <---------->
    // Area case(4)                      <----------->
    // Area case(5)              <----------------------------->
    //
    bool result = true;

    for(auto iter = sparse_list.cbegin(); iter != sparse_list.cend(); ++iter){
        off_t check_start = 0;
        off_t check_bytes = 0;
        if((iter->offset + iter->bytes) <= checkpage.offset){
            // case 0
            continue;    // next

        }else if((checkpage.offset + checkpage.bytes) <= iter->offset){
            // case 1
            break;       // finish

        }else if(iter->offset < checkpage.offset && (iter->offset + iter->bytes) < (checkpage.offset + checkpage.bytes)){
            // case 2
            check_start = checkpage.offset;
            check_bytes = iter->bytes - (checkpage.offset - iter->offset);

        }else if((checkpage.offset + checkpage.bytes) < (iter->offset + iter->bytes)){  // here, already "iter->offset < (checkpage.offset + checkpage.bytes)" is true.
            // case 3
            check_start = iter->offset;
            check_bytes = checkpage.bytes - (iter->offset - checkpage.offset);

        }else if(checkpage.offset < iter->offset && (iter->offset + iter->bytes) < (checkpage.offset + checkpage.bytes)){
            // case 4
            check_start = iter->offset;
            check_bytes = iter->bytes;

        }else{  // (iter->offset <= checkpage.offset && (checkpage.offset + checkpage.bytes) <= (iter->offset + iter->bytes))
            // case 5
            check_start = checkpage.offset;
            check_bytes = checkpage.bytes;
        }

        // check target area type
        if(checkpage.loaded || checkpage.modified){
            // target area must be not HOLE(DATA) area.
            if(!iter->loaded){
                // Found bad area, it is HOLE area.
                fdpage page(check_start, check_bytes, false, false);
                err_area_list.push_back(page);
                result = false;
            }
        }else{
            // target area should be HOLE area.(If it is not a block boundary, it may be a DATA area.)
            if(iter->loaded){
                // need to check this area's each data, it should be ZERO.
                if(!PageList::CheckZeroAreaInFile(fd, check_start, static_cast<size_t>(check_bytes))){
                    // Discovered an area that has un-initial status data but it probably does not effect bad.
                    fdpage page(check_start, check_bytes, true, false);
                    warn_area_list.push_back(page);
                    result = false;
                }
            }
        }
    }
    return result;
}

//------------------------------------------------
// PageList methods
//------------------------------------------------
void PageList::FreeList(fdpage_list_t& list)
{
    list.clear();
}

PageList::PageList(off_t size, bool is_loaded, bool is_modified, bool shrunk) : is_shrink(shrunk)
{
    Init(size, is_loaded, is_modified);
}

PageList::~PageList()
{
    Clear();
}

void PageList::Clear()
{
    PageList::FreeList(pages);
    is_shrink = false;
}

bool PageList::Init(off_t size, bool is_loaded, bool is_modified)
{
    Clear();
    if(0 <= size){
        fdpage page(0, size, is_loaded, is_modified);
        pages.push_back(page);
    }
    return true;
}

off_t PageList::Size() const
{
    if(pages.empty()){
        return 0;
    }
    auto riter = pages.rbegin();
    return riter->next();
}

bool PageList::Compress()
{
    fdpage* lastpage = nullptr;
    for(auto iter = pages.begin(); iter != pages.end(); ){
        if(!lastpage){
            // First item
            lastpage = &(*iter);
            ++iter;
        }else{
            // check page continuity
            if(lastpage->next() != iter->offset){
                // Non-consecutive with last page, so add a page filled with default values
                if(lastpage->loaded || lastpage->modified){
                    // insert new page before current pos
                    fdpage tmppage(lastpage->next(), (iter->offset - lastpage->next()), false, false);
                    iter = pages.insert(iter, tmppage);
                    lastpage = &(*iter);
                    ++iter;
                }else{
                    // Expand last area
                    lastpage->bytes = iter->offset - lastpage->offset;
                }
            }
            // check current page
            if(lastpage->loaded == iter->loaded && lastpage->modified == iter->modified){
                // Expand last area and remove current pos
                lastpage->bytes += iter->bytes;
                iter = pages.erase(iter);
            }else{
                lastpage = &(*iter);
                ++iter;
            }
        }
    }
    return true;
}

bool PageList::Parse(off_t new_pos)
{
    for(auto iter = pages.begin(); iter != pages.end(); ++iter){
        if(new_pos == iter->offset){
            // nothing to do
            return true;
        }else if(iter->offset < new_pos && new_pos < iter->next()){
            fdpage page(iter->offset, new_pos - iter->offset, iter->loaded, iter->modified);
            iter->bytes -= (new_pos - iter->offset);
            iter->offset = new_pos;
            pages.insert(iter, page);
            return true;
        }
    }
    return false;
}

bool PageList::Resize(off_t size, bool is_loaded, bool is_modified)
{
    off_t total = Size();

    if(0 == total){
        // [NOTE]
        // The is_shrink flag remains unchanged in this function.
        //
        bool backup_is_shrink = is_shrink;

        Init(size, is_loaded, is_modified);
        is_shrink = backup_is_shrink;

    }else if(total < size){
        // add new area
        fdpage page(total, (size - total), is_loaded, is_modified);
        pages.push_back(page);

    }else if(size < total){
        // cut area
        for(auto iter = pages.begin(); iter != pages.end(); ){
            if(iter->next() <= size){
                ++iter;
            }else{
                if(size <= iter->offset){
                    iter = pages.erase(iter);
                }else{
                    iter->bytes = size - iter->offset;
                }
            }
        }
        if(is_modified){
            is_shrink = true;
        }
    }else{    // total == size
        // nothing to do
    }
    // compress area
    return Compress();
}

bool PageList::IsPageLoaded(off_t start, off_t size) const
{
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->end() < start){
            continue;
        }
        if(!iter->loaded){
            return false;
        }
        if(0 != size && start + size <= iter->next()){
            break;
        }
    }
    return true;
}

bool PageList::SetPageLoadedStatus(off_t start, off_t size, PageList::page_status pstatus, bool is_compress)
{
    off_t now_size    = Size();
    bool  is_loaded   = (page_status::LOAD_MODIFIED == pstatus || page_status::LOADED == pstatus);
    bool  is_modified = (page_status::LOAD_MODIFIED == pstatus || page_status::MODIFIED == pstatus);

    if(now_size <= start){
        if(now_size < start){
            // add
            Resize(start, false, is_modified);   // set modified flag from now end pos to specified start pos.
        }
        Resize(start + size, is_loaded, is_modified);

    }else if(now_size <= start + size){
        // cut
        Resize(start, false, false);            // not changed loaded/modified flags in existing area.
        // add
        Resize(start + size, is_loaded, is_modified);

    }else{
        // start-size are inner pages area
        // parse "start", and "start + size" position
        Parse(start);
        Parse(start + size);

        // set loaded flag
        for(auto iter = pages.begin(); iter != pages.end(); ++iter){
            if(iter->end() < start){
                continue;
            }else if(start + size <= iter->offset){
                break;
            }else{
                iter->loaded   = is_loaded;
                iter->modified = is_modified;
            }
        }
    }
    // compress area
    return (is_compress ? Compress() : true);
}

bool PageList::FindUnloadedPage(off_t start, off_t& resstart, off_t& ressize) const
{
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(start <= iter->end()){
            if(!iter->loaded && !iter->modified){     // Do not load unloaded and modified areas
                resstart = iter->offset;
                ressize  = iter->bytes;
                return true;
            }
        }
    }
    return false;
}

// [NOTE]
// Accumulates the range of unload that is smaller than the Limit size.
// If you want to integrate all unload ranges, set the limit size to 0.
//
off_t PageList::GetTotalUnloadedPageSize(off_t start, off_t size, off_t limit_size) const
{
    // If size is 0, it means loading to end.
    if(0 == size){
        if(start < Size()){
            size = Size() - start;
        }
    }
    off_t next     = start + size;
    off_t restsize = 0;
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->next() <= start){
            continue;
        }
        if(next <= iter->offset){
            break;
        }
        if(iter->loaded || iter->modified){
            continue;
        }
        off_t tmpsize;
        if(iter->offset <= start){
            if(iter->next() <= next){
                tmpsize = (iter->next() - start);
            }else{
                tmpsize = next - start;                  // = size
            }
        }else{
            if(iter->next() <= next){
                tmpsize = iter->next() - iter->offset;   // = iter->bytes
            }else{
                tmpsize = next - iter->offset;
            }
        }
        if(0 == limit_size || tmpsize < limit_size){
            restsize += tmpsize;
        }
    }
    return restsize;
}

size_t PageList::GetUnloadedPages(fdpage_list_t& unloaded_list, off_t start, off_t size) const
{
    // If size is 0, it means loading to end.
    if(0 == size){
        if(start < Size()){
            size = Size() - start;
        }
    }
    off_t next = start + size;

    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->next() <= start){
            continue;
        }
        if(next <= iter->offset){
            break;
        }
        if(iter->loaded || iter->modified){
            continue; // already loaded or modified
        }

        // page area
        off_t page_start = std::max(iter->offset, start);
        off_t page_next  = std::min(iter->next(), next);
        off_t page_size  = page_next - page_start;

        // add list
        auto riter = unloaded_list.rbegin();
        if(riter != unloaded_list.rend() && riter->next() == page_start){
            // merge to before page
            riter->bytes += page_size;
        }else{
            fdpage page(page_start, page_size, false, false);
            unloaded_list.push_back(page);
        }
    }
    return unloaded_list.size();
}

// [NOTE]
// This method is called in advance when mixing POST and COPY in multi-part upload.
// The minimum size of each part must be 5 MB, and the data area below this must be
// downloaded from S3.
// This method checks the current PageList status and returns the area that needs
// to be downloaded so that each part is at least 5 MB.
//
bool PageList::GetPageListsForMultipartUpload(fdpage_list_t& dlpages, fdpage_list_t& mixuppages, off_t max_partsize)
{
    // compress before this processing
    Compress();         // always true

    // make a list by modified flag
    fdpage_list_t modified_pages;
    fdpage_list_t download_pages;         // A non-contiguous page list showing the areas that need to be downloaded
    fdpage_list_t mixupload_pages;        // A continuous page list showing only modified flags for mixupload
    compress_fdpage_list_ignore_load(pages, modified_pages, false);

    fdpage        prev_page;
    for(auto iter = modified_pages.cbegin(); iter != modified_pages.cend(); ++iter){
        if(iter->modified){
            // current is modified area
            if(!prev_page.modified){
                // previous is not modified area
                if(prev_page.bytes < MIN_MULTIPART_SIZE){
                    // previous(not modified) area is too small for one multipart size,
                    // then all of previous area is needed to download.
                    download_pages.push_back(prev_page);

                    // previous(not modified) area is set upload area.
                    prev_page.modified = true;
                    mixupload_pages.push_back(prev_page);
                }else{
                    // previous(not modified) area is set copy area.
                    prev_page.modified = false;
                    mixupload_pages.push_back(prev_page);
                }
                // set current to previous
                prev_page = *iter;
            }else{
                // previous is modified area, too
                prev_page.bytes += iter->bytes;
            }

        }else{
            // current is not modified area
            if(!prev_page.modified){
                // previous is not modified area, too
                prev_page.bytes += iter->bytes;

            }else{
                // previous is modified area
                if(prev_page.bytes < MIN_MULTIPART_SIZE){
                    // previous(modified) area is too small for one multipart size,
                    // then part or all of current area is needed to download.
                    off_t  missing_bytes = MIN_MULTIPART_SIZE - prev_page.bytes;

                    if((missing_bytes + MIN_MULTIPART_SIZE) < iter-> bytes){
                        // The current size is larger than the missing size, and the remainder
                        // after deducting the missing size is larger than the minimum size.

                        fdpage missing_page(iter->offset, missing_bytes, false, false);
                        download_pages.push_back(missing_page);

                        // previous(not modified) area is set upload area.
                        prev_page.bytes = MIN_MULTIPART_SIZE;
                        mixupload_pages.push_back(prev_page);

                        // set current to previous
                        prev_page = *iter;
                        prev_page.offset += missing_bytes;
                        prev_page.bytes  -= missing_bytes;

                    }else{
                        // The current size is less than the missing size, or the remaining
                        // size less the missing size is less than the minimum size.
                        download_pages.push_back(*iter);

                        // add current to previous
                        prev_page.bytes += iter->bytes;
                    }

                }else{
                    // previous(modified) area is enough size for one multipart size.
                    mixupload_pages.push_back(prev_page);

                    // set current to previous
                    prev_page = *iter;
                }
            }
        }
    }
    // last area
    if(0 < prev_page.bytes){
        mixupload_pages.push_back(prev_page);
    }

    // compress
    compress_fdpage_list_ignore_modify(download_pages, dlpages, false);
    compress_fdpage_list_ignore_load(mixupload_pages, mixuppages, false);

    // parse by max pagesize
    dlpages    = parse_partsize_fdpage_list(dlpages, max_partsize);
    mixuppages = parse_partsize_fdpage_list(mixuppages, max_partsize);

    return true;
}

bool PageList::GetNoDataPageLists(fdpage_list_t& nodata_pages, off_t start, size_t size)
{
    // compress before this processing
    Compress();         // always true

    // extract areas without data
    fdpage_list_t tmp_pagelist;
    off_t         stop_pos = (0L == size ? -1 : (start + size));
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if((iter->offset + iter->bytes) < start){
            continue;
        }
        if(-1 != stop_pos && stop_pos <= iter->offset){
            break;
        }
        if(iter->modified){
            continue;
        }

        fdpage  tmppage;
        tmppage.offset   = std::max(iter->offset, start);
        tmppage.bytes    = (-1 == stop_pos ? iter->bytes : std::min(iter->bytes, (stop_pos - tmppage.offset)));
        tmppage.loaded   = iter->loaded;
        tmppage.modified = iter->modified;

        tmp_pagelist.push_back(tmppage);
    }

    if(tmp_pagelist.empty()){
        nodata_pages.clear();
    }else{
        // compress
        compress_fdpage_list(tmp_pagelist, nodata_pages);
    }
    return true;
}

off_t PageList::BytesModified() const
{
    off_t total = 0;
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->modified){
            total += iter->bytes;
        }
    }
    return total;
}

bool PageList::IsModified() const
{
    if(is_shrink){
        return true;
    }
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(iter->modified){
            return true;
        }
    }
    return false;
}

bool PageList::ClearAllModified()
{
    is_shrink = false;

    for(auto iter = pages.begin(); iter != pages.end(); ++iter){
        if(iter->modified){
            iter->modified = false;
        }
    }
    return Compress();
}

bool PageList::Serialize(CacheFileStat& file, ino_t inode)
{
    if(!file.Open()){
        return false;
    }

    //
    // put to file
    //
    std::ostringstream ssall;
    ssall << inode << ":" << Size();

    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        ssall << "\n" << iter->offset << ":" << iter->bytes << ":" << (iter->loaded ? "1" : "0") << ":" << (iter->modified ? "1" : "0");
    }

    if(-1 == ftruncate(file.GetFd(), 0)){
        S3FS_PRN_ERR("failed to truncate file(to 0) for stats(%d)", errno);
        return false;
    }
    std::string strall = ssall.str();
    if(0 >= pwrite(file.GetFd(), strall.c_str(), strall.length(), 0)){
        S3FS_PRN_ERR("failed to write stats(%d)", errno);
        return false;
    }

    return true;
}

bool PageList::Deserialize(CacheFileStat& file, ino_t inode)
{
    if(!file.Open()){
        return false;
    }

    //
    // loading from file
    //
    struct stat st{};
    if(-1 == fstat(file.GetFd(), &st)){
        S3FS_PRN_ERR("fstat is failed. errno(%d)", errno);
        return false;
    }
    if(0 >= st.st_size){
        // nothing
        Init(0, false, false);
        return true;
    }
    auto ptmp = std::make_unique<char[]>(st.st_size + 1);
    ssize_t result;
    // read from file
    if(0 >= (result = pread(file.GetFd(), ptmp.get(), st.st_size, 0))){
        S3FS_PRN_ERR("failed to read stats(%d)", errno);
        return false;
    }
    ptmp[result] = '\0';
    std::string        oneline;
    std::istringstream ssall(ptmp.get());

    // loaded
    Clear();

    // load head line(for size and inode)
    off_t total;
    ino_t cache_inode;                  // if this value is 0, it means old format.
    if(!getline(ssall, oneline, '\n')){
        S3FS_PRN_ERR("failed to parse stats.");
        return false;
    }else{
        std::istringstream sshead(oneline);
        std::string        strhead1;
        std::string        strhead2;

        // get first part in head line.
        if(!getline(sshead, strhead1, ':')){
            S3FS_PRN_ERR("failed to parse stats.");
            return false;
        }
        // get second part in head line.
        if(!getline(sshead, strhead2, ':')){
            // old head format is "<size>\n"
            total       = cvt_strtoofft(strhead1.c_str(), /* base= */10);
            cache_inode = 0;
        }else{
            // current head format is "<inode>:<size>\n"
            total       = cvt_strtoofft(strhead2.c_str(), /* base= */10);
            cache_inode = static_cast<ino_t>(cvt_strtoofft(strhead1.c_str(), /* base= */10));
            if(0 == cache_inode){
                S3FS_PRN_ERR("wrong inode number in parsed cache stats.");
                return false;
            }
        }
    }
    // check inode number
    if(0 != cache_inode && cache_inode != inode){
        S3FS_PRN_ERR("differ inode and inode number in parsed cache stats.");
        return false;
    }

    // load each part
    bool is_err = false;
    while(getline(ssall, oneline, '\n')){
        std::string        part;
        std::istringstream ssparts(oneline);
        // offset
        if(!getline(ssparts, part, ':')){
            is_err = true;
            break;
        }
        off_t offset = cvt_strtoofft(part.c_str(), /* base= */10);
        // size
        if(!getline(ssparts, part, ':')){
            is_err = true;
            break;
        }
        off_t size = cvt_strtoofft(part.c_str(), /* base= */10);
        // loaded
        if(!getline(ssparts, part, ':')){
            is_err = true;
            break;
        }
        bool is_loaded = (1 == cvt_strtoofft(part.c_str(), /* base= */10) ? true : false);
        bool is_modified;
        if(!getline(ssparts, part, ':')){
            is_modified = false;        // old version does not have this part.
        }else{
            is_modified = (1 == cvt_strtoofft(part.c_str(), /* base= */10) ? true : false);
        }
        // add new area
        PageList::page_status pstatus = PageList::page_status::NOT_LOAD_MODIFIED;
        if(is_loaded){
            if(is_modified){
                pstatus = PageList::page_status::LOAD_MODIFIED;
            }else{
                pstatus = PageList::page_status::LOADED;
            }
        }else{
            if(is_modified){
                pstatus = PageList::page_status::MODIFIED;
            }
        }
        SetPageLoadedStatus(offset, size, pstatus);
    }
    if(is_err){
        S3FS_PRN_ERR("failed to parse stats.");
        Clear();
        return false;
    }

    // check size
    if(total != Size()){
        S3FS_PRN_ERR("different size(%lld - %lld).", static_cast<long long int>(total), static_cast<long long int>(Size()));
        Clear();
        return false;
    }

    return true;
}

void PageList::Dump() const
{
    int cnt = 0;

    S3FS_PRN_DBG("pages (shrunk=%s) = {", (is_shrink ? "yes" : "no"));
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter, ++cnt){
        S3FS_PRN_DBG("  [%08d] -> {%014lld - %014lld : %s / %s}", cnt, static_cast<long long int>(iter->offset), static_cast<long long int>(iter->bytes), iter->loaded ? "loaded" : "unloaded", iter->modified ? "modified" : "not modified");
    }
    S3FS_PRN_DBG("}");
}

// 
// Compare the fdpage_list_t pages of the object with the state of the file.
// 
// The loaded=true or modified=true area of pages must be a DATA block
// (not a HOLE block) in the file.
// The other area is a HOLE block in the file or is a DATA block(but the
// data of the target area in that block should be ZERO).
// If it is a bad area in the previous case, it will be reported as an error.
// If the latter case does not match, it will be reported as a warning.
// 
bool PageList::CompareSparseFile(int fd, size_t file_size, fdpage_list_t& err_area_list, fdpage_list_t& warn_area_list) const
{
    err_area_list.clear();
    warn_area_list.clear();

    // First, list the block disk allocation area of the cache file.
    // The cache file has holes(sparse file) and no disk block areas
    // are assigned to any holes.
    fdpage_list_t sparse_list;
    if(!PageList::GetSparseFilePages(fd, file_size, sparse_list)){
        S3FS_PRN_ERR("Something error is occurred in parsing hole/data of the cache file(physical_fd=%d).", fd);

        fdpage page(0, static_cast<off_t>(file_size), false, false);
        err_area_list.push_back(page);

        return false;
    }

    if(sparse_list.empty() && pages.empty()){
        // both file and stats information are empty, it means cache file size is ZERO.
        return true;
    }

    // Compare each pages and sparse_list
    bool result = true;
    for(auto iter = pages.cbegin(); iter != pages.cend(); ++iter){
        if(!PageList::CheckAreaInSparseFile(*iter, sparse_list, fd, err_area_list, warn_area_list)){
            result = false;
        }
    }
    return result;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
