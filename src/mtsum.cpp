#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <argparse/argparse.hpp>
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

size_t previousPowerOfTwo(size_t x) {
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >> 16);
    x = x | (x >> 32);
    return x - (x >> 1);
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

MTNode build_tree(tf::Subflow& sbf, Scope& scope, size_t offset, size_t size) {
    if (size <= MT_BLOCK_SIZE) {
        MTNode node(scope.tree);
        auto readAndHash = sbf.emplace(
            [&node, &scope, offset, size]() {
                int bufferIndex = scope.bufferPool.alloc();
                auto& buffer = scope.bufferPool.buffers[bufferIndex];
                readBytesFromFileLLFIO(scope.filePath, offset, buffer.data());
                node.hashFromData({buffer.data(), size});
                scope.bufferPool.free(bufferIndex);
            }
        );

        readAndHash.acquire(scope.semaphore);
        readAndHash.release(scope.semaphore);
        sbf.join();

        return node;
    }

    auto leftOffset = offset;
    auto leftSize = previousPowerOfTwo(size - 1);
    auto rightOffset = offset + leftSize;
    auto rightSize = size - leftSize;

    MTNode node(scope.tree);
    sbf.emplace(
        [&node, &scope, leftOffset, leftSize](tf::Subflow& sbf) {
            node.left = std::make_unique<MTNode>(build_tree(sbf, scope, leftOffset, leftSize));
        }
    ).name(std::to_string(leftOffset) + ", " + std::to_string(leftSize));

    sbf.emplace(
        [&node, &scope, rightOffset, rightSize](tf::Subflow& sbf) {
            node.right = std::make_unique<MTNode>(build_tree(sbf, scope, rightOffset, rightSize));
        }
    ).name(std::to_string(rightOffset) + ", " + std::to_string(rightSize));

    sbf.join();

    node.hashFromChildren();

    return node;
}

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("mtsum", "1.0.0");
    program.set_usage_max_line_width(200);

    program.add_argument("-p")
        .help("number of processors to use, default is 8")
        .metavar("processors")
        .default_value(8)
        .scan<'i', int>();

    program.add_argument("-a")
        .help("algorithm to use, default is sha256")
        .metavar("algorithm")
        .choices("md5", "sha1", "sha256", "sha384", "sha512")
        .default_value("sha256");

    program.add_argument("path")
        .help("path to input file")
        .required();

    program.add_group("Misc options");
    program.add_argument("-b")
        .help("enable benchmark")
        .flag();

    program.add_argument("-v")
        .help("enable verbose output")
        .flag();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    auto p = program.get<int>("-p");
    auto algorithmName = program.get<std::string>("-a");
    auto filePath = program.get<std::string>("path");
    auto verbose = program.get<bool>("-v");
    auto benchmark = program.get<bool>("-b");

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);

    if (!file) {
        std::cerr << "Error opening file: " << filePath << std::endl;
        return 1;
    }

    if (p < 1) {
        std::cerr << "Number of processors must be at least 1" << std::endl;
        return 1;
    }

    auto algorithm = EVP_get_digestbyname(algorithmName.c_str());

    if (!algorithm) {
        std::cerr << "Invalid algorithm: " << algorithmName << std::endl;
        return 1;
    }

    size_t fileSize = file.tellg();

    if (verbose) {
        std::cout << "Algorithm: " << algorithmName << std::endl;
        std::cout << "Number of processors: " << p << std::endl;
        std::cout << "File size: " << fileSize << " bytes" << std::endl;
    }

    auto t0 = std::chrono::system_clock::now();
    MTTree tree(algorithm);

    tf::Executor executor(p);
    tf::Taskflow taskflow("merkel_tree");

    Scope scope(tree, filePath, p);

    auto allocTask = taskflow.for_each_index(
        0, scope.bufferCount, 1, [&scope](int i) {
            scope.bufferPool.buffers[i].resize(MT_BLOCK_SIZE);
        }
    );
    auto rootTask = taskflow.emplace(
        [&scope, fileSize](tf::Subflow& sbf) {
            scope.tree.root = std::make_unique<MTNode>(build_tree(sbf, scope, 0, fileSize));
        }
    );
    allocTask.precede(rootTask);
    executor.run(taskflow).wait();

    auto t1 = std::chrono::system_clock::now();
    auto elapsed_par = std::chrono::duration<double>(t1 - t0);

    for (int i = 0; i < tree.hashSize; i++) {
        std::cout << std::hex << (int) tree.root->hash[i];
    }
    std::cout << std::endl;

    if (verbose || benchmark) {
        std::cout << std::fixed << std::setprecision(2) << elapsed_par.count() << " s (";
        double gbPerSecond = (static_cast<double>(fileSize) / 1e9) / static_cast<double>(elapsed_par.count());
        std::cout << std::fixed << std::setprecision(2) << gbPerSecond << " GB/s)" << std::endl;
    }

//    taskflow.dump(std::cout);

    return 0;
}