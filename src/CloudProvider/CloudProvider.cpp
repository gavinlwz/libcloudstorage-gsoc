/*****************************************************************************
 * CloudProvider.cpp : implementation of CloudProvider
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

#include "CloudProvider.h"

#include <json/json.h>
#include <cstring>
#include <fstream>
#include <sstream>

#include "Utility/Item.h"
#include "Utility/Utility.h"

#include "Request/CreateDirectoryRequest.h"
#include "Request/DeleteItemRequest.h"
#include "Request/DownloadFileRequest.h"
#include "Request/ExchangeCodeRequest.h"
#include "Request/GetItemDataRequest.h"
#include "Request/GetItemRequest.h"
#include "Request/ListDirectoryPageRequest.h"
#include "Request/ListDirectoryRequest.h"
#include "Request/MoveItemRequest.h"
#include "Request/RenameItemRequest.h"
#include "Request/UploadFileRequest.h"

#ifdef WITH_CRYPTOPP
#include "Utility/CryptoPP.h"
#endif

#ifdef WITH_CURL
#include "Utility/CurlHttp.h"
#endif

#ifdef WITH_MICROHTTPD
#include "Utility/MicroHttpdServer.h"
#endif

using namespace std::placeholders;

const std::string DEFAULT_STATE = "DEFAULT_STATE";

namespace {

class ListDirectoryCallback : public cloudstorage::IListDirectoryCallback {
 public:
  ListDirectoryCallback(cloudstorage::ListDirectoryCallback callback)
      : callback_(callback) {}

  void receivedItem(cloudstorage::IItem::Pointer) override {}

  void done(cloudstorage::EitherError<std::vector<cloudstorage::IItem::Pointer>>
                result) override {
    callback_(result);
  }

 private:
  cloudstorage::ListDirectoryCallback callback_;
};

class DownloadFileCallback : public cloudstorage::IDownloadFileCallback {
 public:
  DownloadFileCallback(const std::string& filename,
                       cloudstorage::DownloadFileCallback callback)
      : file_(filename, std::ios_base::out | std::ios_base::binary),
        callback_(callback) {}

  void receivedData(const char* data, uint32_t length) override {
    file_.write(data, length);
  }

  void done(cloudstorage::EitherError<void> e) override {
    file_.close();
    callback_(e);
  }

  void progress(uint32_t, uint32_t) override {}

 private:
  std::fstream file_;
  cloudstorage::DownloadFileCallback callback_;
};

class UploadFileCallback : public cloudstorage::IUploadFileCallback {
 public:
  UploadFileCallback(const std::string& path,
                     cloudstorage::UploadFileCallback callback)
      : file_(path, std::ios_base::in | std::ios_base::binary),
        callback_(callback) {
    file_.seekg(0, std::ios::end);
    size_ = file_.tellg();
    file_.seekg(std::ios::beg);
  }

  void reset() override {
    file_.clear();
    file_.seekg(std::ios::beg);
  }

  uint32_t putData(char* data, uint32_t maxlength) override {
    file_.read(data, maxlength);
    return file_.gcount();
  }

  uint64_t size() override { return size_; }

  void done(cloudstorage::EitherError<void> e) override { callback_(e); }

  void progress(uint32_t, uint32_t) override {}

 private:
  std::fstream file_;
  cloudstorage::UploadFileCallback callback_;
  uint64_t size_;
};

}  // namespace

namespace cloudstorage {

CloudProvider::CloudProvider(IAuth::Pointer auth)
    : auth_(std::move(auth)), http_() {}

void CloudProvider::initialize(InitData&& data) {
  auto lock = auth_lock();
  callback_ = std::move(data.callback_);
  crypto_ = std::move(data.crypto_engine_);
  http_ = std::move(data.http_engine_);
  http_server_ = std::move(data.http_server_);

  auto t = auth()->fromTokenString(data.token_);
  setWithHint(data.hints_, "access_token",
              [&t](std::string v) { t->token_ = v; });
  auth()->set_access_token(std::move(t));

  setWithHint(data.hints_, "client_id",
              [this](std::string v) { auth()->set_client_id(v); });
  setWithHint(data.hints_, "client_secret",
              [this](std::string v) { auth()->set_client_secret(v); });
  setWithHint(data.hints_, "redirect_uri",
              [this](std::string v) { auth()->set_redirect_uri(v); });
  setWithHint(data.hints_, "state",
              [this](std::string v) { auth()->set_state(v); });
  setWithHint(data.hints_, "login_page",
              [this](std::string v) { auth()->set_login_page(v); });
  setWithHint(data.hints_, "success_page",
              [this](std::string v) { auth()->set_success_page(v); });
  setWithHint(data.hints_, "error_page",
              [this](std::string v) { auth()->set_error_page(v); });

#ifdef WITH_CRYPTOPP
  if (!crypto_) crypto_ = util::make_unique<CryptoPP>();
#endif

#ifdef WITH_CURL
  if (!http_) http_ = util::make_unique<curl::CurlHttp>();
#endif

#ifdef WITH_MICROHTTPD
  if (!http_server_)
    http_server_ = util::make_unique<MicroHttpdServerFactory>();
#endif

  if (!http_) throw std::runtime_error("No http module specified.");
  if (!http_server_)
    throw std::runtime_error("No http server module specified.");
  if (auth()->state().empty()) auth()->set_state(DEFAULT_STATE);
  auth()->initialize(http(), http_server());
}

std::string ICloudProvider::serializeSession(const std::string& token,
                                             const Hints& hints) {
  Json::Value root_json;
  Json::Value hints_json;
  for (const auto& hint : hints) {
    hints_json[hint.first] = hint.second;
  }
  root_json["hints"] = hints_json;
  root_json["token"] = token;

  Json::FastWriter fastWriter;
  fastWriter.omitEndingLineFeed();
  return fastWriter.write(root_json);
}

bool ICloudProvider::deserializeSession(const std::string& serialized_data,
                                        std::string& token, Hints& hints) {
  std::string token_tmp;
  Hints hints_tmp;
  Json::Reader reader;
  Json::Value unserialized_json;

  if (!reader.parse(serialized_data, unserialized_json)) return false;
  try {
    for (const auto& key : unserialized_json["hints"]) {
      std::string hint_key = key.asString();
      hints_tmp[hint_key] = unserialized_json["hints"][hint_key].asString();
    }
    token_tmp = unserialized_json["token"].asString();
  } catch (const std::runtime_error&) {
    return false;
  }
  token = token_tmp;
  hints = hints_tmp;
  return true;
}

ICloudProvider::Hints CloudProvider::hints() const {
  return {{"access_token", access_token()}, {"state", auth()->state()}};
}

std::string CloudProvider::access_token() const {
  auto lock = auth_lock();
  if (auth()->access_token() == nullptr) return "";
  return auth()->access_token()->token_;
}

IAuth* CloudProvider::auth() const { return auth_.get(); }

std::string CloudProvider::authorizeLibraryUrl() const {
  return auth()->authorizeLibraryUrl();
}

std::string CloudProvider::token() const {
  auto lock = auth_lock();
  if (auth()->access_token() == nullptr) return "";
  return auth()->access_token()->refresh_token_;
}

IItem::Pointer CloudProvider::rootDirectory() const {
  return util::make_unique<Item>("/", "root", IItem::UnknownSize,
                                 IItem::FileType::Directory);
}

ICloudProvider::IAuthCallback* CloudProvider::auth_callback() const {
  return callback_.get();
}

ICrypto* CloudProvider::crypto() const { return crypto_.get(); }

IHttp* CloudProvider::http() const { return http_.get(); }

IHttpServerFactory* CloudProvider::http_server() const {
  return http_server_.get();
}

ICloudProvider::ExchangeCodeRequest::Pointer CloudProvider::exchangeCodeAsync(
    const std::string& code, ExchangeCodeCallback callback) {
  return std::make_shared<cloudstorage::ExchangeCodeRequest>(shared_from_this(),
                                                             code, callback)
      ->run();
}

ICloudProvider::ListDirectoryRequest::Pointer CloudProvider::listDirectoryAsync(
    IItem::Pointer item, IListDirectoryCallback::Pointer callback) {
  return std::make_shared<cloudstorage::ListDirectoryRequest>(
             shared_from_this(), std::move(item), std::move(callback))
      ->run();
}

ICloudProvider::GetItemRequest::Pointer CloudProvider::getItemAsync(
    const std::string& absolute_path, GetItemCallback callback) {
  return std::make_shared<cloudstorage::GetItemRequest>(shared_from_this(),
                                                        absolute_path, callback)
      ->run();
}

ICloudProvider::DownloadFileRequest::Pointer CloudProvider::downloadFileAsync(
    IItem::Pointer file, IDownloadFileCallback::Pointer callback, Range range) {
  return std::make_shared<cloudstorage::DownloadFileRequest>(
             shared_from_this(), std::move(file), std::move(callback), range,
             std::bind(&CloudProvider::downloadFileRequest, this, _1, _2))
      ->run();
}

ICloudProvider::UploadFileRequest::Pointer CloudProvider::uploadFileAsync(
    IItem::Pointer directory, const std::string& filename,
    IUploadFileCallback::Pointer callback) {
  return std::make_shared<cloudstorage::UploadFileRequest>(
             shared_from_this(), std::move(directory), filename,
             std::move(callback))
      ->run();
}

ICloudProvider::GetItemDataRequest::Pointer CloudProvider::getItemDataAsync(
    const std::string& id, GetItemDataCallback f) {
  return std::make_shared<cloudstorage::GetItemDataRequest>(shared_from_this(),
                                                            id, f)
      ->run();
}

void CloudProvider::authorizeRequest(IHttpRequest& r) const {
  r.setHeaderParameter("Authorization", "Bearer " + access_token());
}

bool CloudProvider::reauthorize(int code) const {
  return IHttpRequest::isAuthorizationError(code);
}

bool CloudProvider::unpackCredentials(const std::string&) { return false; }

AuthorizeRequest::Pointer CloudProvider::authorizeAsync() {
  return std::make_shared<AuthorizeRequest>(shared_from_this());
}

std::unique_lock<std::mutex> CloudProvider::auth_lock() const {
  return std::unique_lock<std::mutex>(auth_mutex_);
}

void CloudProvider::setWithHint(const ICloudProvider::Hints& hints,
                                const std::string& name,
                                std::function<void(std::string)> f) const {
  auto it = hints.find(name);
  if (it != hints.end()) f(it->second);
}

std::string CloudProvider::getPath(const std::string& p) {
  std::string result = p;
  if (result.back() == '/') result.pop_back();
  return result.substr(0, result.find_last_of('/'));
}

std::string CloudProvider::getFilename(const std::string& path) {
  std::string result = path;
  if (result.back() == '/') result.pop_back();
  return result.substr(result.find_last_of('/') + 1);
}

std::pair<std::string, std::string> CloudProvider::credentialsFromString(
    const std::string& str) {
  auto it = str.find(Auth::SEPARATOR);
  if (it == std::string::npos) return {};
  std::string login(str.begin(), str.begin() + it);
  std::string password(str.begin() + it + strlen(Auth::SEPARATOR), str.end());
  return {login, password};
}

ICloudProvider::DownloadFileRequest::Pointer CloudProvider::getThumbnailAsync(
    IItem::Pointer item, IDownloadFileCallback::Pointer callback) {
  return std::make_shared<cloudstorage::DownloadFileRequest>(
             shared_from_this(), item, std::move(callback), FullRange,
             std::bind(&CloudProvider::getThumbnailRequest, this, _1, _2))
      ->run();
}

ICloudProvider::DeleteItemRequest::Pointer CloudProvider::deleteItemAsync(
    IItem::Pointer item, DeleteItemCallback callback) {
  return std::make_shared<cloudstorage::DeleteItemRequest>(shared_from_this(),
                                                           item, callback)
      ->run();
}

ICloudProvider::CreateDirectoryRequest::Pointer
CloudProvider::createDirectoryAsync(IItem::Pointer parent,
                                    const std::string& name,
                                    CreateDirectoryCallback callback) {
  return std::make_shared<cloudstorage::CreateDirectoryRequest>(
             shared_from_this(), parent, name, callback)
      ->run();
}

ICloudProvider::MoveItemRequest::Pointer CloudProvider::moveItemAsync(
    IItem::Pointer source, IItem::Pointer destination,
    MoveItemCallback callback) {
  return std::make_shared<cloudstorage::MoveItemRequest>(
             shared_from_this(), source, destination, callback)
      ->run();
}

ICloudProvider::RenameItemRequest::Pointer CloudProvider::renameItemAsync(
    IItem::Pointer item, const std::string& name, RenameItemCallback callback) {
  return std::make_shared<cloudstorage::RenameItemRequest>(shared_from_this(),
                                                           item, name, callback)
      ->run();
}

ICloudProvider::ListDirectoryPageRequest::Pointer
CloudProvider::listDirectoryPageAsync(IItem::Pointer directory,
                                      const std::string& token,
                                      ListDirectoryPageCallback completed) {
  return std::make_shared<cloudstorage::ListDirectoryPageRequest>(
             shared_from_this(), directory, token, completed)
      ->run();
}

ICloudProvider::ListDirectoryRequest::Pointer CloudProvider::listDirectoryAsync(
    IItem::Pointer item, ListDirectoryCallback callback) {
  return listDirectoryAsync(
      item, util::make_unique<::ListDirectoryCallback>(callback));
}

ICloudProvider::DownloadFileRequest::Pointer CloudProvider::downloadFileAsync(
    IItem::Pointer item, const std::string& filename,
    DownloadFileCallback callback) {
  return downloadFileAsync(
      item, util::make_unique<::DownloadFileCallback>(filename, callback),
      FullRange);
}

ICloudProvider::DownloadFileRequest::Pointer CloudProvider::getThumbnailAsync(
    IItem::Pointer item, const std::string& filename,
    GetThumbnailCallback callback) {
  return getThumbnailAsync(
      item, util::make_unique<::DownloadFileCallback>(filename, callback));
}

ICloudProvider::UploadFileRequest::Pointer CloudProvider::uploadFileAsync(
    IItem::Pointer parent, const std::string& path, const std::string& filename,
    UploadFileCallback callback) {
  return uploadFileAsync(
      parent, filename,
      util::make_unique<::UploadFileCallback>(path, callback));
}

IHttpRequest::Pointer CloudProvider::getItemDataRequest(const std::string&,
                                                        std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::listDirectoryRequest(const IItem&,
                                                          const std::string&,
                                                          std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::uploadFileRequest(const IItem&,
                                                       const std::string&,
                                                       std::ostream&,
                                                       std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::downloadFileRequest(const IItem&,
                                                         std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::getThumbnailRequest(const IItem& i,
                                                         std::ostream&) const {
  const Item& item = static_cast<const Item&>(i);
  if (item.thumbnail_url().empty()) return nullptr;
  return http()->create(item.thumbnail_url(), "GET");
}

IHttpRequest::Pointer CloudProvider::deleteItemRequest(const IItem&,
                                                       std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::createDirectoryRequest(
    const IItem&, const std::string&, std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::moveItemRequest(const IItem&, const IItem&,
                                                     std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer CloudProvider::renameItemRequest(const IItem&,
                                                       const std::string&,
                                                       std::ostream&) const {
  return nullptr;
}

IItem::Pointer CloudProvider::getItemDataResponse(std::istream&) const {
  return nullptr;
}

std::vector<IItem::Pointer> CloudProvider::listDirectoryResponse(
    const IItem&, std::istream&, std::string&) const {
  return {};
}

IItem::Pointer CloudProvider::createDirectoryResponse(
    std::istream& stream) const {
  return getItemDataResponse(stream);
}

}  // namespace cloudstorage
