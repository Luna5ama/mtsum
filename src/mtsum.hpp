#pragma once

#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>
#include <llfio.hpp>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include "merkel_tree.hpp"

struct Scope {
    MTTree& tree;
    int bufferCount;
    std::string filePath;
    tf::Semaphore semaphore;
    BufferPool bufferPool;

    Scope(MTTree& tree, std::string filePath, int p)
        : tree(tree), bufferCount(p), filePath(std::move(filePath)), semaphore(p), bufferPool(bufferCount) {}
};

size_t floorPot(size_t x) {
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >> 16);
    x = x | (x >> 32);
    return x - (x >> 1);
}

size_t ceilBlockSize(size_t x) {
    return ((x + MT_BLOCK_SIZE - 1) / MT_BLOCK_SIZE) * MT_BLOCK_SIZE;
}

void readBytesFromFileLLFIO(const std::string& filePath, size_t readOffset, uint8_t* outputBuffer) {
    namespace llfio = LLFIO_V2_NAMESPACE;
    try {
        llfio::file_handle fh = llfio::file(
            {},
            filePath,
            llfio::file_handle::mode::read,
            llfio::file_handle::creation::open_existing,
            llfio::file_handle::caching::only_metadata
        ).value();
        llfio::read(
            fh,
            readOffset,
            {{reinterpret_cast<llfio::byte*>(outputBuffer), MT_BLOCK_SIZE}}
        );
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

MTNode buildTree(tf::Subflow& sbf, Scope& scope, size_t offset, size_t size);

MTNode computeLeafHash(Scope& scope, size_t offset, size_t size) {
    MTNode node(scope.tree);
    int bufferIndex = scope.bufferPool.alloc();
    auto& buffer = scope.bufferPool.buffers[bufferIndex];
    readBytesFromFileLLFIO(scope.filePath, offset, buffer.data());
    node.hashFromData({buffer.data(), size});
    scope.bufferPool.free(bufferIndex);
    return node;
}

tf::Task makeChildNode(tf::Subflow& sbf, Scope& scope, std::unique_ptr<MTNode>& child, size_t offset, size_t size) {
    if (size <= MT_BLOCK_SIZE) {
        return sbf.emplace(
                [&scope, &child, offset, size](tf::Subflow& sbf) {
                    child = std::make_unique<MTNode>(computeLeafHash(scope, offset, size));
                }
            )
            .acquire(scope.semaphore)
            .release(scope.semaphore)
            .name(std::to_string(offset) + ", " + std::to_string(size));
    } else {
        return sbf.emplace(
            [&scope, &child, offset, size](tf::Subflow& sbf) {
                child = std::make_unique<MTNode>(buildTree(sbf, scope, offset, size));
            }
        ).name(std::to_string(offset) + ", " + std::to_string(size));
    }
}

MTNode buildTree(tf::Subflow& sbf, Scope& scope, size_t offset, size_t size) {
    auto leftOffset = offset;
    auto leftSize = size <= MT_BLOCK_BALANCE_THRESHOLD ? floorPot(size - 1) : ceilBlockSize(size / 2);
    auto rightOffset = offset + leftSize;
    auto rightSize = size - leftSize;

    MTNode node(scope.tree);

    auto leftTask = makeChildNode(sbf, scope, node.left, leftOffset, leftSize);
    auto rightTask = makeChildNode(sbf, scope, node.right, rightOffset, rightSize);

    sbf.join();
    leftTask.name(node.left->hashString());
    rightTask.name(node.right->hashString());
    node.hashFromChildren();

    return node;
}