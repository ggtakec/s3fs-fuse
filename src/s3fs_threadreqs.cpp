/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Randy Rizun <rrizun@gmail.com>
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

#include "common.h"
#include "s3fs.h"
#include "s3fs_threadreqs.h"
#include "threadpoolman.h"
#include "s3fs_logger.h"
#include "s3fs_util.h"
#include "cache.h"
#include "string_util.h"

//-------------------------------------------------------------------
// Thread Worker functions for MultiThread Request
//-------------------------------------------------------------------
//
// Thread Worker function for head request
//
void* head_req_threadworker(void* arg)
{
    head_req_thparam* pthparam = static_cast<head_req_thparam*>(arg);
    if(!pthparam || !pthparam->pmeta){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Head Request [path=%s][pmeta=%p]", pthparam->path.c_str(), pthparam->pmeta);

    S3fsCurl s3fscurl;
    pthparam->result = s3fscurl.HeadRequest(pthparam->path.c_str(), *(pthparam->pmeta));

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for multi head request
//
void* multi_head_req_threadworker(void* arg)
{
    std::unique_ptr<multi_head_req_thparam> pthparam(static_cast<multi_head_req_thparam*>(arg));
    if(!pthparam || !pthparam->psyncfiller || !pthparam->pthparam_lock || !pthparam->pretrycount || !pthparam->pnotfound_list || !pthparam->presult){
        return reinterpret_cast<void*>(-EIO);
    }

    // Check retry max count and print debug message
    {
        const std::lock_guard<std::mutex> lock(*(pthparam->pthparam_lock));

        S3FS_PRN_INFO3("Multi Head Request [filler=%p][thparam_lock=%p][retrycount=%d][notfound_list=%p][wtf8=%s][path=%s]", pthparam->psyncfiller, pthparam->pthparam_lock, *(pthparam->pretrycount), pthparam->pnotfound_list, pthparam->use_wtf8 ? "true" : "false", pthparam->path.c_str());

        if(S3fsCurl::GetRetries() < *(pthparam->pretrycount)){
            S3FS_PRN_ERR("Head request(%s) reached the maximum number of retry count(%d).", pthparam->path.c_str(), *(pthparam->pretrycount));
            return reinterpret_cast<void*>(-EIO);
        }
    }

    // loop for head request
    S3fsCurl  s3fscurl;
    int       result = 0;
    headers_t meta;         // this value is not used
    while(true){
        // Request
        result = s3fscurl.HeadRequest(pthparam->path.c_str(), meta);

        // Check result
        bool     isResetOffset= true;
        CURLcode curlCode     = s3fscurl.GetCurlCode();
        long     responseCode = S3fsCurl::S3FSCURL_RESPONSECODE_NOTSET;
        s3fscurl.GetResponseCode(responseCode, false);

        if(CURLE_OK == curlCode){
            if(responseCode < 400){
                // add into stat cache
                if(StatCache::getStatCacheData()->AddStat(pthparam->path, *(s3fscurl.GetResponseHeaders()))){
                    // Get stats from stats cache(for converting from meta), and fill
                    std::string bpath = mybasename(pthparam->path);
                    if(pthparam->use_wtf8){
                        bpath = s3fs_wtf8_decode(bpath);
                    }

                    struct stat st;
                    if(StatCache::getStatCacheData()->GetStat(pthparam->path, &st)){
                        pthparam->psyncfiller->Fill(bpath, &st, 0);
                    }else{
                        S3FS_PRN_INFO2("Could not find %s file in stat cache.", pthparam->path.c_str());
                        pthparam->psyncfiller->Fill(bpath, nullptr, 0);
                    }
                    result = 0;
                }else{
                    S3FS_PRN_ERR("failed adding stat cache [path=%s]", pthparam->path.c_str());
                    if(0 == result){
                        result = -EIO;
                    }
                }
                break;

            }else if(responseCode == 400){
                // as possibly in multipart
                S3FS_PRN_WARN("Head Request(%s) got 400 response code.", pthparam->path.c_str());

            }else if(responseCode == 404){
                // set path to not found list
                S3FS_PRN_INFO("Head Request(%s) got NotFound(404), it maybe only the path exists and the object does not exist.", pthparam->path.c_str());
                {
                    const std::lock_guard<std::mutex> lock(*(pthparam->pthparam_lock));
                    pthparam->pnotfound_list->push_back(pthparam->path);
                }
                break;

            }else if(responseCode == 500){
                // case of all other result, do retry.(11/13/2013)
                // because it was found that s3fs got 500 error from S3, but could success
                // to retry it.
                S3FS_PRN_WARN("Head Request(%s) got 500 response code.", pthparam->path.c_str());

            // cppcheck-suppress unmatchedSuppression
            // cppcheck-suppress knownConditionTrueFalse
            }else if(responseCode == S3fsCurl::S3FSCURL_RESPONSECODE_NOTSET){
                // This is a case where the processing result has not yet been updated (should be very rare).
                S3FS_PRN_WARN("Head Request(%s) could not get any response code.", pthparam->path.c_str());

            }else{  // including S3fsCurl::S3FSCURL_RESPONSECODE_FATAL_ERROR
                // Retry in other case.
                S3FS_PRN_WARN("Head Request(%s) got fatal response code.", pthparam->path.c_str());
            }

        }else if(CURLE_OPERATION_TIMEDOUT == curlCode){
            S3FS_PRN_ERR("Head Request(%s) is timeouted.", pthparam->path.c_str());
            isResetOffset= false;

        }else if(CURLE_PARTIAL_FILE == curlCode){
            S3FS_PRN_WARN("Head Request(%s) is recieved data does not match the given size.", pthparam->path.c_str());
            isResetOffset= false;

        }else{
            S3FS_PRN_WARN("Head Request(%s) got the result code(%d: %s)", pthparam->path.c_str(), curlCode, curl_easy_strerror(curlCode));
        }

        // Check retry max count
        {
            const std::lock_guard<std::mutex> lock(*(pthparam->pthparam_lock));

            ++(*(pthparam->pretrycount));
            if(S3fsCurl::GetRetries() < *(pthparam->pretrycount)){
                S3FS_PRN_ERR("Head request(%s) reached the maximum number of retry count(%d).", pthparam->path.c_str(), *(pthparam->pretrycount));
                if(0 == result){
                    result = -EIO;
                }
                break;
            }
        }

        // Setup for retry
        if(isResetOffset){
            S3fsCurl::ResetOffset(&s3fscurl);
        }
    }

    // Set result code
    {
        const std::lock_guard<std::mutex> lock(*(pthparam->pthparam_lock));
        if(0 == *(pthparam->presult) && 0 != result){
            // keep first error
            *(pthparam->presult) = result;
        }
    }

    // [NOTE]
    // The return value of a Multi Head request thread will always be 0(nullptr).
    // This is because the expected value of a Head request will always be a
    // response other than 200, such as 400/404/etc.
    // In those error cases, this function simply outputs a message. And those
    // errors(the first one) will be set to pthparam->presult and can be referenced
    // by the caller.
    //
    return nullptr;
}

//
// Thread Worker function for delete request
//
void* delete_req_threadworker(void* arg)
{
    delete_req_thparam* pthparam = static_cast<delete_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Delete Request [path=%s]", pthparam->path.c_str());

    S3fsCurl s3fscurl;
    pthparam->result = s3fscurl.DeleteRequest(pthparam->path.c_str());

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for put head request
//
void* put_head_req_threadworker(void* arg)
{
    put_head_req_thparam* pthparam = static_cast<put_head_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Put Head Request [path=%s][meta count=%lu][is copy=%s][use_ahbe=%s]", pthparam->path.c_str(), pthparam->meta.size(), (pthparam->isCopy ? "true" : "false"), (pthparam->ahbe ? "true" : "false"));

    S3fsCurl s3fscurl(pthparam->ahbe);
    pthparam->result = s3fscurl.PutHeadRequest(pthparam->path.c_str(), pthparam->meta, pthparam->isCopy);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for put request
//
void* put_req_threadworker(void* arg)
{
    put_req_thparam* pthparam = static_cast<put_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Put Request [path=%s][meta count=%lu][fd=%d][use_ahbe=%s]", pthparam->path.c_str(), pthparam->meta.size(), pthparam->fd, (pthparam->ahbe ? "true" : "false"));

    S3fsCurl s3fscurl(pthparam->ahbe);
    pthparam->result = s3fscurl.PutRequest(pthparam->path.c_str(), pthparam->meta, pthparam->fd);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for list bucket request
//
void* list_bucket_req_threadworker(void* arg)
{
    list_bucket_req_thparam* pthparam = static_cast<list_bucket_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("List Bucket Request [path=%s][query=%s]", pthparam->path.c_str(), pthparam->each_query.c_str());

    S3fsCurl s3fscurl;
    if(0 == (pthparam->result = s3fscurl.ListBucketRequest(pthparam->path.c_str(), pthparam->each_query.c_str()))){
        pthparam->responseBody = s3fscurl.GetBodyData();
    }
    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for check service request
//
void* check_service_req_threadworker(void* arg)
{
    check_service_req_thparam* pthparam = static_cast<check_service_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Check Service Request [path=%s][support compat dir=%s][force No SSE=%s]", pthparam->path.c_str(), (pthparam->support_compat_dir ? "true" : "false"), (pthparam->forceNoSSE ? "true" : "false"));

    S3fsCurl s3fscurl;
    pthparam->result       = s3fscurl.CheckBucket(pthparam->path.c_str(), pthparam->support_compat_dir, pthparam->forceNoSSE);
    pthparam->responseCode = s3fscurl.GetLastResponseCode();
    pthparam->responseBody = s3fscurl.GetBodyData();

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Worker function for pre multipart post request
//
void* pre_multipart_post_req_threadworker(void* arg)
{
    pre_multipart_post_req_thparam* pthparam = static_cast<pre_multipart_post_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Pre Multipart Post Request [path=%s][meta count=%lu][is copy=%s]", pthparam->path.c_str(), pthparam->meta.size(), (pthparam->is_copy ? "true" : "false"));

    S3fsCurl s3fscurl(true);
    pthparam->result = s3fscurl.PreMultipartPostRequest(pthparam->path.c_str(), pthparam->meta, pthparam->upload_id, pthparam->is_copy);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Worker function for complete multipart post request
//
void* complete_multipart_post_threadworker(void* arg)
{
    complete_multipart_post_req_thparam* pthparam = static_cast<complete_multipart_post_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Complete Multipart Post Request [path=%s][upload id=%s][etaglist=%lu]", pthparam->path.c_str(), pthparam->upload_id.c_str(), pthparam->etaglist.size());

    S3fsCurl s3fscurl(true);
    pthparam->result = s3fscurl.CompleteMultipartPostRequest(pthparam->path.c_str(), pthparam->upload_id, pthparam->etaglist);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Worker function for abort multipart upload request
//
void* abort_multipart_upload_req_threadworker(void* arg)
{
    abort_multipart_upload_req_thparam* pthparam = static_cast<abort_multipart_upload_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Abort Multipart Upload Request [path=%s][upload id=%s]", pthparam->path.c_str(), pthparam->upload_id.c_str());

    S3fsCurl s3fscurl(true);
    pthparam->result = s3fscurl.AbortMultipartUpload(pthparam->path.c_str(), pthparam->upload_id);

    return reinterpret_cast<void*>(pthparam->result);
}

//-------------------------------------------------------------------
// Utility functions
//-------------------------------------------------------------------
//
// Calls S3fsCurl::PreMultipartPostRequest via pre_multipart_post_req_threadworker
//
// [NOTE]
// If the request is successful, sets upload_id.
//
int pre_multipart_post_request(const std::string& path, const headers_t& meta, bool is_copy, std::string& upload_id)
{
    // parameter for thread worker
    pre_multipart_post_req_thparam thargs;
    thargs.path    = path;
    thargs.meta    = meta;              // copy
    thargs.upload_id.clear();           // clear
    thargs.is_copy = is_copy;
    thargs.result  = 0;

    // make parameter for thread pool
    thpoolman_param  ppoolparam;
    ppoolparam.args  = &thargs;
    ppoolparam.psem  = nullptr;         // case await
    ppoolparam.pfunc = pre_multipart_post_req_threadworker;

    // send request by thread
    if(!ThreadPoolMan::AwaitInstruct(ppoolparam)){
        S3FS_PRN_ERR("failed to setup Pre Multipart Post Request Thread Worker");
        return -EIO;
    }
    if(0 == thargs.result){
        // set upload_id
        upload_id = thargs.upload_id;
    }else{
        S3FS_PRN_ERR("Pre Multipart Post Request(path=%s) returns with error(%d)", path.c_str(), thargs.result);
    }

    return thargs.result;
}

//
// Calls S3fsCurl::CompleteMultipartPostRequest via complete_multipart_post_threadworker
//
int complete_multipart_post_request(const std::string& path, const std::string& upload_id, const etaglist_t& parts)
{
    // parameter for thread worker
    complete_multipart_post_req_thparam thargs;
    thargs.path      = path;
    thargs.upload_id = upload_id;
    thargs.etaglist  = parts;           // copy
    thargs.result    = 0;

    // make parameter for thread pool
    thpoolman_param  ppoolparam;
    ppoolparam.args  = &thargs;
    ppoolparam.psem  = nullptr;         // case await
    ppoolparam.pfunc = complete_multipart_post_threadworker;

    // send request by thread
    if(!ThreadPoolMan::AwaitInstruct(ppoolparam)){
        S3FS_PRN_ERR("failed to setup Complete Multipart Post Request Thread Worker");
        return -EIO;
    }
    if(0 != thargs.result){
        S3FS_PRN_ERR("Complete Multipart Post Request(path=%s) returns with error(%d)", path.c_str(), thargs.result);
        return thargs.result;
    }
    return 0;
}

//
// Calls S3fsCurl::AbortMultipartUpload via abort_multipart_upload_req_threadworker
//
int abort_multipart_upload_request(const std::string& path, const std::string& upload_id)
{
    // parameter for thread worker
    abort_multipart_upload_req_thparam thargs;
    thargs.path      = path;
    thargs.upload_id = upload_id;
    thargs.result    = 0;

    // make parameter for thread pool
    thpoolman_param  ppoolparam;
    ppoolparam.args  = &thargs;
    ppoolparam.psem  = nullptr;         // case await
    ppoolparam.pfunc = abort_multipart_upload_req_threadworker;

    // send request by thread
    if(!ThreadPoolMan::AwaitInstruct(ppoolparam)){
        S3FS_PRN_ERR("failed to setup Abort Multipart Upload Request Thread Worker");
        return -EIO;
    }
    if(0 != thargs.result){
        S3FS_PRN_ERR("Abort Multipart Upload Request(path=%s) returns with error(%d)", path.c_str(), thargs.result);
    }
    return thargs.result;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
