/*****************************************************************************
 * CurlHttp.h : interface of CurlHttp
 *
 *****************************************************************************
 * Copyright (C) 2017-2017 VideoLAN
 *
 * Authors: Diogo Silva <dbtdsilva@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "MicroHttpd.h"

namespace cloudstorage {
namespace {
int iterateOverConnectionValues(void *cls, enum MHD_ValueKind kind, 
        const char *key, const char *value) {
    //fprintf(stderr, "Key: %s, Value: %s\n", key, value);
    IHttpd::ConnectionValues *data = static_cast<IHttpd::ConnectionValues*>(cls);
    if (data->find(key) == data->end()) {
        data->insert( std::make_pair(key, value));
        return MHD_YES; // Keep iterating
    }
    return MHD_NO;  // Stop iterating
}
}
    
int MicroHttpd::MHDRequestCallback(void* cls, MHD_Connection* connection, 
        const char* url, const char* /*method*/, const char* /*version*/,
        const char* /*upload_data*/, size_t* /*upload_size*/, void** /*ptr*/) 
{
    CallbackData* data = static_cast<CallbackData*>(cls);
    
    RequestData request_data;
    request_data.url.assign(url);
    request_data.custom_data = data->custom_data;
    
    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, 
            &iterateOverConnectionValues, &(request_data.args));
    MHD_get_connection_values(connection, MHD_HEADER_KIND, 
            &iterateOverConnectionValues, &(request_data.headers));
    
    std::string response_page = data->request_callback(&request_data);
    
    MHD_Response* mdh_response = MHD_create_response_from_buffer(
            response_page.length(), (void*)response_page.c_str(), 
            MHD_RESPMEM_MUST_COPY);
    int status = MHD_queue_response((MHD_Connection*) connection, 
            MHD_HTTP_OK, mdh_response);
    MHD_destroy_response(mdh_response);
    return status;
}

void MicroHttpd::startServer(uint16_t port, 
        CallbackFunction request_callback, void* data)
{
    callback_data.request_callback = request_callback;
    callback_data.custom_data = data;
    http_server = MHD_start_daemon(MHD_USE_POLL_INTERNALLY, port, NULL, NULL, 
            &MicroHttpd::MHDRequestCallback, &callback_data, MHD_OPTION_END);
}

void MicroHttpd::stopServer()
{
    MHD_stop_daemon(http_server);
}

}  // namespace cloudstorage

