// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <system_error>

#include "io/io.h"
#include "util/cloud/aws.h"
#include "util/http/http_client.h"

namespace util {
namespace cloud {

using ListBucketsResult = io::Result<std::vector<std::string>>;

ListBucketsResult ListS3Buckets(const AWS& aws, http::Client* http_client);

class S3Bucket {
 public:
  S3Bucket(const AWS& aws, std::string_view bucket);

  std::error_code Connect(uint32_t ms);

  //! Called with (size, key_name) pairs.
  using ListObjectCb = std::function<void(size_t, std::string_view)>;

  std::error_code ListObjects(std::string_view path, ListObjectCb cb);

 private:
  std::string GetHost() const;

  AWS aws_;
  std::string bucket_;
  std::unique_ptr<http::Client> http_client_;
};

}  // namespace cloud

}  // namespace util