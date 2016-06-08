/*****************************************************************************
 * Dropbox.cpp : implementation of Dropbox
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

#include "Dropbox.h"

#include <jsoncpp/json/json.h>
#include <sstream>

#include "HttpRequest.h"
#include "Item.h"
#include "Utility.h"

namespace cloudstorage {

Dropbox::Dropbox() : CloudProvider(make_unique<Auth>()) {}

std::vector<IItem::Pointer> Dropbox::executeListDirectory(
    const IItem& f) const {
  const Item& item = static_cast<const Item&>(f);
  HttpRequest request("https://api.dropboxapi.com/2/files/list_folder",
                      HttpRequest::Type::POST);
  request.setHeaderParameter("Authorization",
                             std::string("Bearer ") + access_token());
  request.setHeaderParameter("Content-Type", "application/json");

  std::vector<IItem::Pointer> result;
  std::string cursor;
  while (true) {
    Json::Value parameter;
    if (!cursor.empty())
      parameter["cursor"] = cursor;
    else
      parameter["path"] = item.id();
    request.set_post_data(parameter.toStyledString());
    Json::Value response;
    std::stringstream(request.send()) >> response;
    for (Json::Value v : response["entries"]) {
      result.push_back(make_unique<Item>(v["name"].asString(),
                                         v["path_display"].asString(),
                                         v[".tag"].asString() == "folder"));
    }
    if (!response["has_more"].asBool()) break;

    request.set_url("https://api.dropboxapi.com/2/files/list_folder/continue");
    cursor = response["cursor"].asString();
  }
  return result;
}

std::string Dropbox::name() const { return "dropbox"; }

IItem::Pointer Dropbox::rootDirectory() const {
  return make_unique<Item>("/", "", true);
}

void Dropbox::executeUploadFile(const std::string&, std::istream&) const {
  // TODO
}

void Dropbox::executeDownloadFile(const IItem&, std::ostream&) const {
  // TODO
}

Dropbox::Auth::Auth() {
  set_client_id("ktryxp68ae5cicj");
  set_client_secret("6evu94gcxnmyr59");
}

std::string Dropbox::Auth::authorizeLibraryUrl() const {
  std::string url = "https://www.dropbox.com/oauth2/authorize?";
  url += "response_type=code&";
  url += "client_id=" + client_id() + "&";
  url += "redirect_uri=" + redirect_uri() + "&";
  return url;
}

std::string Dropbox::Auth::requestAuthorizationCode() const {
  return awaitAuthorizationCode("code", "error");
}

IAuth::Token::Pointer Dropbox::Auth::requestAccessToken() const {
  HttpRequest request("https://api.dropboxapi.com/oauth2/token",
                      HttpRequest::Type::POST);
  request.setParameter("grant_type", "authorization_code");
  request.setParameter("client_id", client_id());
  request.setParameter("client_secret", client_secret());
  request.setParameter("redirect_uri", redirect_uri());
  request.setParameter("code", authorization_code());

  Json::Value response;
  std::stringstream(request.send()) >> response;

  Token::Pointer token = make_unique<Token>();
  token->token_ = response["access_token"].asString();
  token->expires_in_ = -1;
  return token;
}

IAuth::Token::Pointer Dropbox::Auth::refreshToken() const {
  if (!access_token()) return nullptr;
  Token::Pointer token = make_unique<Token>();
  token->token_ = access_token()->token_;
  token->expires_in_ = -1;
  return token;
}

bool Dropbox::Auth::validateToken(IAuth::Token& token) const {
  Dropbox dropbox;
  dropbox.auth()->set_access_token(make_unique<Token>(token));
  try {
    dropbox.listDirectory(*dropbox.rootDirectory());
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

}  // namespace cloudstorage