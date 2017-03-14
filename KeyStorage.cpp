/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KeyStorage.h"

#include "Keymaster.h"
#include "ScryptParameters.h"
#include "Utils.h"

#include <vector>

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <android-base/file.h>
#include <android-base/logging.h>

#include <cutils/properties.h>

#include <hardware/hw_auth_token.h>

#include <keystore/authorization_set.h>
#include <keystore/keystore_hidl_support.h>

extern "C" {

#include "crypto_scrypt.h"
}

namespace android {
namespace vold {
using namespace keystore;

const KeyAuthentication kEmptyAuthentication{"", ""};

static constexpr size_t AES_KEY_BYTES = 32;
static constexpr size_t GCM_NONCE_BYTES = 12;
static constexpr size_t GCM_MAC_BYTES = 16;
static constexpr size_t SALT_BYTES = 1 << 4;
static constexpr size_t SECDISCARDABLE_BYTES = 1 << 14;
static constexpr size_t STRETCHED_BYTES = 1 << 6;

static constexpr uint32_t AUTH_TIMEOUT = 30; // Seconds

static const char* kCurrentVersion = "1";
static const char* kRmPath = "/system/bin/rm";
static const char* kSecdiscardPath = "/system/bin/secdiscard";
static const char* kStretch_none = "none";
static const char* kStretch_nopassword = "nopassword";
static const std::string kStretchPrefix_scrypt = "scrypt ";
static const char* kHashPrefix_secdiscardable = "Android secdiscardable SHA512";
static const char* kHashPrefix_keygen = "Android key wrapping key generation SHA512";
static const char* kFn_encrypted_key = "encrypted_key";
static const char* kFn_keymaster_key_blob = "keymaster_key_blob";
static const char* kFn_keymaster_key_blob_upgraded = "keymaster_key_blob_upgraded";
static const char* kFn_salt = "salt";
static const char* kFn_secdiscardable = "secdiscardable";
static const char* kFn_stretching = "stretching";
static const char* kFn_version = "version";

static bool checkSize(const std::string& kind, size_t actual, size_t expected) {
    if (actual != expected) {
        LOG(ERROR) << "Wrong number of bytes in " << kind << ", expected " << expected << " got "
                   << actual;
        return false;
    }
    return true;
}

static std::string hashWithPrefix(char const* prefix, const std::string& tohash) {
    SHA512_CTX c;

    SHA512_Init(&c);
    // Personalise the hashing by introducing a fixed prefix.
    // Hashing applications should use personalization except when there is a
    // specific reason not to; see section 4.11 of https://www.schneier.com/skein1.3.pdf
    std::string hashingPrefix = prefix;
    hashingPrefix.resize(SHA512_CBLOCK);
    SHA512_Update(&c, hashingPrefix.data(), hashingPrefix.size());
    SHA512_Update(&c, tohash.data(), tohash.size());
    std::string res(SHA512_DIGEST_LENGTH, '\0');
    SHA512_Final(reinterpret_cast<uint8_t*>(&res[0]), &c);
    return res;
}

static bool generateKeymasterKey(Keymaster& keymaster, const KeyAuthentication& auth,
                                 const std::string& appId, std::string* key) {
    auto paramBuilder = AuthorizationSetBuilder()
                            .AesEncryptionKey(AES_KEY_BYTES * 8)
                            .Authorization(TAG_BLOCK_MODE, BlockMode::GCM)
                            .Authorization(TAG_MIN_MAC_LENGTH, GCM_MAC_BYTES * 8)
                            .Authorization(TAG_PADDING, PaddingMode::NONE)
                            .Authorization(TAG_APPLICATION_ID, blob2hidlVec(appId));
    if (auth.token.empty()) {
        LOG(DEBUG) << "Creating key that doesn't need auth token";
        paramBuilder.Authorization(TAG_NO_AUTH_REQUIRED);
    } else {
        LOG(DEBUG) << "Auth token required for key";
        if (auth.token.size() != sizeof(hw_auth_token_t)) {
            LOG(ERROR) << "Auth token should be " << sizeof(hw_auth_token_t) << " bytes, was "
                       << auth.token.size() << " bytes";
            return false;
        }
        const hw_auth_token_t* at = reinterpret_cast<const hw_auth_token_t*>(auth.token.data());
        paramBuilder.Authorization(TAG_USER_SECURE_ID, at->user_id);
        paramBuilder.Authorization(TAG_USER_AUTH_TYPE, HardwareAuthenticatorType::PASSWORD);
        paramBuilder.Authorization(TAG_AUTH_TIMEOUT, AUTH_TIMEOUT);
    }
    return keymaster.generateKey(paramBuilder, key);
}

static AuthorizationSet beginParams(const KeyAuthentication& auth,
                                               const std::string& appId) {
    auto paramBuilder = AuthorizationSetBuilder()
                            .Authorization(TAG_BLOCK_MODE, BlockMode::GCM)
                            .Authorization(TAG_MAC_LENGTH, GCM_MAC_BYTES * 8)
                            .Authorization(TAG_PADDING, PaddingMode::NONE)
                            .Authorization(TAG_APPLICATION_ID, blob2hidlVec(appId));
    if (!auth.token.empty()) {
        LOG(DEBUG) << "Supplying auth token to Keymaster";
        paramBuilder.Authorization(TAG_AUTH_TOKEN, blob2hidlVec(auth.token));
    }
    return paramBuilder;
}

static bool readFileToString(const std::string& filename, std::string* result) {
    if (!android::base::ReadFileToString(filename, result)) {
        PLOG(ERROR) << "Failed to read from " << filename;
        return false;
    }
    return true;
}

static bool writeStringToFile(const std::string& payload, const std::string& filename) {
    if (!android::base::WriteStringToFile(payload, filename)) {
        PLOG(ERROR) << "Failed to write to " << filename;
        return false;
    }
    return true;
}

static KeymasterOperation begin(Keymaster& keymaster, const std::string& dir,
                                KeyPurpose purpose,
                                const AuthorizationSet &keyParams,
                                const AuthorizationSet &opParams,
                                AuthorizationSet* outParams) {
    auto kmKeyPath = dir + "/" + kFn_keymaster_key_blob;
    std::string kmKey;
    if (!readFileToString(kmKeyPath, &kmKey)) return KeymasterOperation();
    AuthorizationSet inParams(keyParams);
    inParams.append(opParams.begin(), opParams.end());
    for (;;) {
        auto opHandle = keymaster.begin(purpose, kmKey, inParams, outParams);
        if (opHandle) {
            return opHandle;
        }
        if (opHandle.errorCode() != ErrorCode::KEY_REQUIRES_UPGRADE) return opHandle;
        LOG(DEBUG) << "Upgrading key: " << dir;
        std::string newKey;
        if (!keymaster.upgradeKey(kmKey, keyParams, &newKey)) return KeymasterOperation();
        auto newKeyPath = dir + "/" + kFn_keymaster_key_blob_upgraded;
        if (!writeStringToFile(newKey, newKeyPath)) return KeymasterOperation();
        if (rename(newKeyPath.c_str(), kmKeyPath.c_str()) != 0) {
            PLOG(ERROR) << "Unable to move upgraded key to location: " << kmKeyPath;
            return KeymasterOperation();
        }
        if (!keymaster.deleteKey(kmKey)) {
            LOG(ERROR) << "Key deletion failed during upgrade, continuing anyway: " << dir;
        }
        kmKey = newKey;
        LOG(INFO) << "Key upgraded: " << dir;
    }
}

static bool encryptWithKeymasterKey(Keymaster& keymaster, const std::string& dir,
                                    const AuthorizationSet &keyParams,
                                    const std::string& message, std::string* ciphertext) {
    AuthorizationSet opParams;
    AuthorizationSet outParams;
    auto opHandle = begin(keymaster, dir, KeyPurpose::ENCRYPT, keyParams, opParams, &outParams);
    if (!opHandle) return false;
    auto nonceBlob = outParams.GetTagValue(TAG_NONCE);
    if (!nonceBlob.isOk()) {
        LOG(ERROR) << "GCM encryption but no nonce generated";
        return false;
    }
    // nonceBlob here is just a pointer into existing data, must not be freed
    std::string nonce(reinterpret_cast<const char*>(&nonceBlob.value()[0]), nonceBlob.value().size());
    if (!checkSize("nonce", nonce.size(), GCM_NONCE_BYTES)) return false;
    std::string body;
    if (!opHandle.updateCompletely(message, &body)) return false;

    std::string mac;
    if (!opHandle.finish(&mac)) return false;
    if (!checkSize("mac", mac.size(), GCM_MAC_BYTES)) return false;
    *ciphertext = nonce + body + mac;
    return true;
}

static bool decryptWithKeymasterKey(Keymaster& keymaster, const std::string& dir,
                                    const AuthorizationSet &keyParams,
                                    const std::string& ciphertext, std::string* message) {
    auto nonce = ciphertext.substr(0, GCM_NONCE_BYTES);
    auto bodyAndMac = ciphertext.substr(GCM_NONCE_BYTES);
    auto opParams = AuthorizationSetBuilder()
            .Authorization(TAG_NONCE, blob2hidlVec(nonce));
    auto opHandle = begin(keymaster, dir, KeyPurpose::DECRYPT, keyParams, opParams, nullptr);
    if (!opHandle) return false;
    if (!opHandle.updateCompletely(bodyAndMac, message)) return false;
    if (!opHandle.finish(nullptr)) return false;
    return true;
}

static std::string getStretching(const KeyAuthentication& auth) {
    if (!auth.usesKeymaster()) {
        return kStretch_none;
    } else if (auth.secret.empty()) {
        return kStretch_nopassword;
    } else {
        char paramstr[PROPERTY_VALUE_MAX];

        property_get(SCRYPT_PROP, paramstr, SCRYPT_DEFAULTS);
        return std::string() + kStretchPrefix_scrypt + paramstr;
    }
}

static bool stretchingNeedsSalt(const std::string& stretching) {
    return stretching != kStretch_nopassword && stretching != kStretch_none;
}

static bool stretchSecret(const std::string& stretching, const std::string& secret,
                          const std::string& salt, std::string* stretched) {
    if (stretching == kStretch_nopassword) {
        if (!secret.empty()) {
            LOG(WARNING) << "Password present but stretching is nopassword";
            // Continue anyway
        }
        stretched->clear();
    } else if (stretching == kStretch_none) {
        *stretched = secret;
    } else if (std::equal(kStretchPrefix_scrypt.begin(), kStretchPrefix_scrypt.end(),
                          stretching.begin())) {
        int Nf, rf, pf;
        if (!parse_scrypt_parameters(stretching.substr(kStretchPrefix_scrypt.size()).c_str(), &Nf,
                                     &rf, &pf)) {
            LOG(ERROR) << "Unable to parse scrypt params in stretching: " << stretching;
            return false;
        }
        stretched->assign(STRETCHED_BYTES, '\0');
        if (crypto_scrypt(reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
                          reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                          1 << Nf, 1 << rf, 1 << pf,
                          reinterpret_cast<uint8_t*>(&(*stretched)[0]), stretched->size()) != 0) {
            LOG(ERROR) << "scrypt failed with params: " << stretching;
            return false;
        }
    } else {
        LOG(ERROR) << "Unknown stretching type: " << stretching;
        return false;
    }
    return true;
}

static bool generateAppId(const KeyAuthentication& auth, const std::string& stretching,
                          const std::string& salt, const std::string& secdiscardable,
                          std::string* appId) {
    std::string stretched;
    if (!stretchSecret(stretching, auth.secret, salt, &stretched)) return false;
    *appId = hashWithPrefix(kHashPrefix_secdiscardable, secdiscardable) + stretched;
    return true;
}

static bool readRandomBytesOrLog(size_t count, std::string* out) {
    auto status = ReadRandomBytes(count, *out);
    if (status != OK) {
        LOG(ERROR) << "Random read failed with status: " << status;
        return false;
    }
    return true;
}

static void logOpensslError() {
    LOG(ERROR) << "Openssl error: " << ERR_get_error();
}

static bool encryptWithoutKeymaster(const std::string& preKey,
                                    const std::string& plaintext, std::string* ciphertext) {
    auto key = hashWithPrefix(kHashPrefix_keygen, preKey);
    key.resize(AES_KEY_BYTES);
    if (!readRandomBytesOrLog(GCM_NONCE_BYTES, ciphertext)) return false;
    auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL,
            reinterpret_cast<const uint8_t*>(key.data()),
            reinterpret_cast<const uint8_t*>(ciphertext->data()))) {
        logOpensslError();
        return false;
    }
    ciphertext->resize(GCM_NONCE_BYTES + plaintext.size() + GCM_MAC_BYTES);
    int outlen;
    if (1 != EVP_EncryptUpdate(ctx.get(),
        reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES), &outlen,
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size())) {
        logOpensslError();
        return false;
    }
    if (outlen != static_cast<int>(plaintext.size())) {
        LOG(ERROR) << "GCM ciphertext length should be " << plaintext.size() << " was " << outlen;
        return false;
    }
    if (1 != EVP_EncryptFinal_ex(ctx.get(),
        reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES + plaintext.size()), &outlen)) {
        logOpensslError();
        return false;
    }
    if (outlen != 0) {
        LOG(ERROR) << "GCM EncryptFinal should be 0, was " << outlen;
        return false;
    }
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, GCM_MAC_BYTES,
        reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES + plaintext.size()))) {
        logOpensslError();
        return false;
    }
    return true;
}

static bool decryptWithoutKeymaster(const std::string& preKey,
                                    const std::string& ciphertext, std::string* plaintext) {
    if (ciphertext.size() < GCM_NONCE_BYTES + GCM_MAC_BYTES) {
        LOG(ERROR) << "GCM ciphertext too small: " << ciphertext.size();
        return false;
    }
    auto key = hashWithPrefix(kHashPrefix_keygen, preKey);
    key.resize(AES_KEY_BYTES);
    auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL,
            reinterpret_cast<const uint8_t*>(key.data()),
            reinterpret_cast<const uint8_t*>(ciphertext.data()))) {
        logOpensslError();
        return false;
    }
    plaintext->resize(ciphertext.size() - GCM_NONCE_BYTES - GCM_MAC_BYTES);
    int outlen;
    if (1 != EVP_DecryptUpdate(ctx.get(),
        reinterpret_cast<uint8_t*>(&(*plaintext)[0]), &outlen,
        reinterpret_cast<const uint8_t*>(ciphertext.data() + GCM_NONCE_BYTES), plaintext->size())) {
        logOpensslError();
        return false;
    }
    if (outlen != static_cast<int>(plaintext->size())) {
        LOG(ERROR) << "GCM plaintext length should be " << plaintext->size() << " was " << outlen;
        return false;
    }
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, GCM_MAC_BYTES,
        const_cast<void *>(
            reinterpret_cast<const void*>(ciphertext.data() + GCM_NONCE_BYTES + plaintext->size())))) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_DecryptFinal_ex(ctx.get(),
        reinterpret_cast<uint8_t*>(&(*plaintext)[0] + plaintext->size()), &outlen)) {
        logOpensslError();
        return false;
    }
    if (outlen != 0) {
        LOG(ERROR) << "GCM EncryptFinal should be 0, was " << outlen;
        return false;
    }
    return true;
}

bool storeKey(const std::string& dir, const KeyAuthentication& auth, const std::string& key) {
    if (TEMP_FAILURE_RETRY(mkdir(dir.c_str(), 0700)) == -1) {
        PLOG(ERROR) << "key mkdir " << dir;
        return false;
    }
    if (!writeStringToFile(kCurrentVersion, dir + "/" + kFn_version)) return false;
    std::string secdiscardable;
    if (!readRandomBytesOrLog(SECDISCARDABLE_BYTES, &secdiscardable)) return false;
    if (!writeStringToFile(secdiscardable, dir + "/" + kFn_secdiscardable)) return false;
    std::string stretching = getStretching(auth);
    if (!writeStringToFile(stretching, dir + "/" + kFn_stretching)) return false;
    std::string salt;
    if (stretchingNeedsSalt(stretching)) {
        if (ReadRandomBytes(SALT_BYTES, salt) != OK) {
            LOG(ERROR) << "Random read failed";
            return false;
        }
        if (!writeStringToFile(salt, dir + "/" + kFn_salt)) return false;
    }
    std::string appId;
    if (!generateAppId(auth, stretching, salt, secdiscardable, &appId)) return false;
    std::string encryptedKey;
    if (auth.usesKeymaster()) {
        Keymaster keymaster;
        if (!keymaster) return false;
        std::string kmKey;
        if (!generateKeymasterKey(keymaster, auth, appId, &kmKey)) return false;
        if (!writeStringToFile(kmKey, dir + "/" + kFn_keymaster_key_blob)) return false;
        auto keyParams = beginParams(auth, appId);
        if (!encryptWithKeymasterKey(keymaster, dir, keyParams, key, &encryptedKey)) return false;
    } else {
        if (!encryptWithoutKeymaster(appId, key, &encryptedKey)) return false;
    }
    if (!writeStringToFile(encryptedKey, dir + "/" + kFn_encrypted_key)) return false;
    return true;
}

bool retrieveKey(const std::string& dir, const KeyAuthentication& auth, std::string* key) {
    std::string version;
    if (!readFileToString(dir + "/" + kFn_version, &version)) return false;
    if (version != kCurrentVersion) {
        LOG(ERROR) << "Version mismatch, expected " << kCurrentVersion << " got " << version;
        return false;
    }
    std::string secdiscardable;
    if (!readFileToString(dir + "/" + kFn_secdiscardable, &secdiscardable)) return false;
    std::string stretching;
    if (!readFileToString(dir + "/" + kFn_stretching, &stretching)) return false;
    std::string salt;
    if (stretchingNeedsSalt(stretching)) {
        if (!readFileToString(dir + "/" + kFn_salt, &salt)) return false;
    }
    std::string appId;
    if (!generateAppId(auth, stretching, salt, secdiscardable, &appId)) return false;
    std::string encryptedMessage;
    if (!readFileToString(dir + "/" + kFn_encrypted_key, &encryptedMessage)) return false;
    if (auth.usesKeymaster()) {
        Keymaster keymaster;
        if (!keymaster) return false;
        auto keyParams = beginParams(auth, appId);
        if (!decryptWithKeymasterKey(keymaster, dir, keyParams, encryptedMessage, key)) return false;
    } else {
        if (!decryptWithoutKeymaster(appId, encryptedMessage, key)) return false;
    }
    return true;
}

static bool deleteKey(const std::string& dir) {
    std::string kmKey;
    if (!readFileToString(dir + "/" + kFn_keymaster_key_blob, &kmKey)) return false;
    Keymaster keymaster;
    if (!keymaster) return false;
    if (!keymaster.deleteKey(kmKey)) return false;
    return true;
}

static bool runSecdiscard(const std::string& dir) {
    if (ForkExecvp(
            std::vector<std::string>{kSecdiscardPath, "--",
                dir + "/" + kFn_encrypted_key,
                dir + "/" + kFn_keymaster_key_blob,
                dir + "/" + kFn_secdiscardable,
                }) != 0) {
        LOG(ERROR) << "secdiscard failed";
        return false;
    }
    return true;
}

static bool recursiveDeleteKey(const std::string& dir) {
    if (ForkExecvp(std::vector<std::string>{kRmPath, "-rf", dir}) != 0) {
        LOG(ERROR) << "recursive delete failed";
        return false;
    }
    return true;
}

bool destroyKey(const std::string& dir) {
    bool success = true;
    // Try each thing, even if previous things failed.
    success &= deleteKey(dir);
    success &= runSecdiscard(dir);
    success &= recursiveDeleteKey(dir);
    return success;
}

}  // namespace vold
}  // namespace android
