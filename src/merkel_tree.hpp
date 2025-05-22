#pragma once

#include <array>
#include <vector>
#include <memory>
#include <openssl/evp.h>
#include <iomanip>
#include "buffer.hpp"

// OpenSSL engine implementation
#define OPENSSL_ENGINE NULL

struct MTTree;
struct MTNode;

constexpr int64_t MT_BLOCK_SIZE = 128 * 1024 * 1024;

struct OpenSSLFree {
    void operator()(void* ptr) const {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ptr));
    }
};

enum class MTNodeType {
    LEAF,
    INTERNAL,
    ROOT
};


struct MTNode {
    MTTree& tree;
    MTNodeType nodeType;
    std::vector<uint8_t> hash;
    std::unique_ptr<MTNode> left;
    std::unique_ptr<MTNode> right;

    MTNode() = delete;
    explicit MTNode(MTTree& tree, MTNodeType nodeType) : tree(tree), nodeType(nodeType) {}

    bool hashFromChildren();
    bool hashFromData(std::span<const uint8_t> inputData);
    [[nodiscard]] std::string hashString() const;
};

struct MTTree {
    const EVP_MD* hashType;
    int hashSize;
    std::unique_ptr<MTNode> root;
    explicit MTTree(const EVP_MD* hashType) : hashType(hashType), hashSize(EVP_MD_get_size(hashType)) {};
};

inline bool MTNode::hashFromData(std::span<const uint8_t> inputData) {
    std::unique_ptr<EVP_MD_CTX, OpenSSLFree> context(EVP_MD_CTX_new());

    if (context == nullptr) {
        return false;
    }

    if (!EVP_DigestInit_ex(context.get(), this->tree.hashType, nullptr)) {
        return false;
    }

    // Second preimage attack fix from "Certificate Transparency"
    uint8_t prependByte = static_cast<uint8_t>(nodeType);
    if (!EVP_DigestUpdate(context.get(), &prependByte, sizeof(prependByte))) {
        return false;
    }

    if (!EVP_DigestUpdate(context.get(), inputData.data(), inputData.size())) {
        return false;
    }

    unsigned int lengthOfHash = 0;

    this->hash.resize(this->tree.hashSize);

    if (!EVP_DigestFinal_ex(context.get(), this->hash.data(), &lengthOfHash)) {
        return false;
    }

    return true;
}

inline bool MTNode::hashFromChildren() {
    std::vector<uint8_t> concatHash(tree.hashSize * 2);
    std::copy(left->hash.begin(), left->hash.end(), concatHash.begin());
    std::copy(right->hash.begin(), right->hash.end(), concatHash.begin() + tree.hashSize);
    return hashFromData(concatHash);
}

inline std::stringstream& operator<<(std::stringstream& ss, const MTNode& node) {
    for (int i = 0; i < node.tree.hashSize; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int) node.hash[i];
    }
    return ss;
}

inline std::string MTNode::hashString() const {
    std::stringstream ss;
    ss << *this;
    return ss.str();
}