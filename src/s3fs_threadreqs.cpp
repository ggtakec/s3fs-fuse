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
#include "s3fs_threadreqs.h"
#include "threadpoolman.h"
#include "s3fs_logger.h"

//-------------------------------------------------------------------
// Thread Worker functions for MultiThread Request
//-------------------------------------------------------------------
//
// Thread Worker function for head request
//
void* head_req_threadworker(void* arg)
{
    auto* pthparam = static_cast<head_req_thparam*>(arg);
    if(!pthparam || !pthparam->pmeta){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Head Request [path=%s][pmeta=%p]", pthparam->path.c_str(), pthparam->pmeta);

    S3fsCurl s3fscurl;
    pthparam->result = s3fscurl.HeadRequest(pthparam->path.c_str(), *(pthparam->pmeta));

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Thread Worker function for delete request
//
void* delete_req_threadworker(void* arg)
{
    auto* pthparam = static_cast<delete_req_thparam*>(arg);
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
    auto* pthparam = static_cast<put_head_req_thparam*>(arg);
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
    auto* pthparam = static_cast<put_req_thparam*>(arg);
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
    auto* pthparam = static_cast<list_bucket_req_thparam*>(arg);
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
    auto* pthparam = static_cast<check_service_req_thparam*>(arg);
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
// Worker function for pre multipart upload request
//
void* pre_multipart_upload_req_threadworker(void* arg)
{
    auto* pthparam = static_cast<pre_multipart_upload_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Pre Multipart Upload Request [path=%s][meta count=%lu]", pthparam->path.c_str(), pthparam->meta.size());

    S3fsCurl s3fscurl(true);
    pthparam->result = s3fscurl.PreMultipartUploadRequest(pthparam->path.c_str(), pthparam->meta, pthparam->upload_id);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Worker function for complete multipart upload request
//
void* complete_multipart_upload_threadworker(void* arg)
{
    auto* pthparam = static_cast<complete_multipart_upload_req_thparam*>(arg);
    if(!pthparam){
        return reinterpret_cast<void*>(-EIO);
    }
    S3FS_PRN_INFO3("Complete Multipart Upload Request [path=%s][upload id=%s][etaglist=%lu]", pthparam->path.c_str(), pthparam->upload_id.c_str(), pthparam->etaglist.size());

    S3fsCurl s3fscurl(true);
    pthparam->result = s3fscurl.MultipartUploadComplete(pthparam->path.c_str(), pthparam->upload_id, pthparam->etaglist);

    return reinterpret_cast<void*>(pthparam->result);
}

//
// Worker function for abort multipart upload request
//
void* abort_multipart_upload_req_threadworker(void* arg)
{
    auto* pthparam = static_cast<abort_multipart_upload_req_thparam*>(arg);
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
// Calls S3fsCurl::PreMultipartUploadRequest via pre_multipart_upload_req_threadworker
//
// [NOTE]
// If the request is successful, sets upload_id.
//
int pre_multipart_upload_request(const std::string& path, const headers_t& meta, std::string& upload_id)
{
    // parameter for thread worker
    pre_multipart_upload_req_thparam thargs;
    thargs.path    = path;
    thargs.meta    = meta;              // copy
    thargs.upload_id.clear();           // clear
    thargs.result  = 0;

    // make parameter for thread pool
    thpoolman_param  ppoolparam;
    ppoolparam.args  = &thargs;
    ppoolparam.psem  = nullptr;         // case await
    ppoolparam.pfunc = pre_multipart_upload_req_threadworker;

    // send request by thread
    if(!ThreadPoolMan::AwaitInstruct(ppoolparam)){
        S3FS_PRN_ERR("failed to setup Pre Multipart Upload Request Thread Worker");
        return -EIO;
    }
    if(0 == thargs.result){
        // set upload_id
        upload_id = thargs.upload_id;
    }else{
        S3FS_PRN_ERR("Pre Multipart Upload Request(path=%s) returns with error(%d)", path.c_str(), thargs.result);
    }

    return thargs.result;
}

//
// Calls S3fsCurl::MultipartUploadComplete via complete_multipart_upload_threadworker
//
int complete_multipart_upload_request(const std::string& path, const std::string& upload_id, const etaglist_t& parts)
{
    // parameter for thread worker
    complete_multipart_upload_req_thparam thargs;
    thargs.path      = path;
    thargs.upload_id = upload_id;
    thargs.etaglist  = parts;           // copy
    thargs.result    = 0;

    // make parameter for thread pool
    thpoolman_param  ppoolparam;
    ppoolparam.args  = &thargs;
    ppoolparam.psem  = nullptr;         // case await
    ppoolparam.pfunc = complete_multipart_upload_threadworker;

    // send request by thread
    if(!ThreadPoolMan::AwaitInstruct(ppoolparam)){
        S3FS_PRN_ERR("failed to setup Complete Multipart Upload Request Thread Worker");
        return -EIO;
    }
    if(0 != thargs.result){
        S3FS_PRN_ERR("Complete Multipart Upload Request(path=%s) returns with error(%d)", path.c_str(), thargs.result);
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
