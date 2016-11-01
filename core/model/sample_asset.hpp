/*
Copyright Soramitsu Co., Ltd. 2016 All Rights Reserved.
www.soramitsu.co.jp

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef __CORE_DOMAIN_ASSET_HPP_
#define __CORE_DOMAIN_ASSET_HPP_

#include "asset.hpp"

namespace domain {
    namespace asset {

        class SampleAsset : public Asset {
          public:
            int maxQuantity;

            SampleAsset(
              std::string aName,
              std::string aParentDomainName,
              int aMaxQuantity
            ):
              Asset(aName, aParentDomainName),
              maxQuantity(aMaxQuantity)
            {}

        };

    }  // namespace asset
}  // namespace domain

#endif
