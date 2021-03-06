/*****************************************************************************
 * DownloadFileRequest.cpp : DownloadFileRequest implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
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

#include "DownloadFileRequest.h"

#include "CloudProvider/CloudProvider.h"

using namespace std::placeholders;

namespace cloudstorage {

DownloadFileRequest::DownloadFileRequest(std::shared_ptr<CloudProvider> p,
                                         IItem::Pointer file,
                                         ICallback::Pointer callback,
                                         Range range,
                                         RequestFactory request_factory)
    : Request(p),
      stream_wrapper_(
          std::bind(&ICallback::receivedData, callback.get(), _1, _2)) {
  set([=](Request::Pointer request) {
    auto response_stream = std::make_shared<std::ostream>(&stream_wrapper_);
    sendRequest(
        [=](util::Output input) {
          auto request = request_factory(*file, *input);
          if (range != FullRange)
            request->setHeaderParameter("Range", util::range_to_string(range));
          return request;
        },
        [=](EitherError<util::Output> e) {
          if (e.left()) {
            callback->done(e.left());
            request->done(e.left());
          } else {
            callback->done(nullptr);
            request->done(nullptr);
          }
        },
        response_stream, std::bind(&DownloadFileRequest::ICallback::progress,
                                   callback.get(), _1, _2));
  });
}

DownloadFileRequest::~DownloadFileRequest() { cancel(); }

DownloadStreamWrapper::DownloadStreamWrapper(
    std::function<void(const char*, uint32_t)> callback)
    : callback_(std::move(callback)) {}

std::streamsize DownloadStreamWrapper::xsputn(const char_type* data,
                                              std::streamsize length) {
  callback_(data, static_cast<uint32_t>(length));
  return length;
}

}  // namespace cloudstorage
