#include <argparse/argparse.hpp>
#include <mpi.h>
#include "mtsum.hpp"

template <class Tp>
inline void DoNotOptimize(Tp const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

void computeLocalTree(
    tf::Executor& executor,
    MTTree& localTree,
    std::string& filePath,
    int p,
    size_t start,
    size_t end
) {
    Scope scope(localTree, filePath, p);
    tf::Taskflow taskflow("merkel_tree_local");

    auto allocTask = taskflow.for_each_index(
        0, scope.bufferCount, 1, [&scope](int i) {
            scope.bufferPool.buffers[i].resize(MT_BLOCK_SIZE);
        }
    );
    auto rootTask = taskflow.emplace(
        [&scope, start, end](tf::Subflow& sbf) {
            scope.tree.root = std::make_unique<MTNode>(buildTree(sbf, scope, start, end));
        }
    );
    allocTask.name("setup");
    rootTask.name("root");
    allocTask.precede(rootTask);
    executor.run(taskflow).wait();
    rootTask.name(scope.tree.root->hashString());
}

void partition(
    std::vector<std::pair<size_t, size_t>>& partitions,
    size_t offset,
    size_t size,
    int currLevelPartitionCount,
    int targetLevelPartitionCount
) {
    if (currLevelPartitionCount == targetLevelPartitionCount) {
        partitions.emplace_back(offset, size);
        return;
    }

    auto leftOffset = offset;
    auto leftSize = ceilBlockSize(size / 2);
    auto rightOffset = offset + leftSize;
    auto rightSize = size - leftSize;
    partition(partitions, leftOffset, leftSize, currLevelPartitionCount << 1, targetLevelPartitionCount);
    partition(partitions, rightOffset, rightSize, currLevelPartitionCount << 1, targetLevelPartitionCount);
}

MTNode globalHash(
    MTTree& globalTree,
    std::vector<uint8_t>& localHashes,
    bool verbose,
    int index,
    int currLevelPartitionCount,
    int targetLevelPartitionCount
) {
    if (currLevelPartitionCount == targetLevelPartitionCount) {
        auto node = MTNode(globalTree);
        node.hash.resize(globalTree.hashSize);
        std::copy(
            localHashes.data() + index * globalTree.hashSize,
            localHashes.data() + (index + 1) * globalTree.hashSize,
            node.hash.data()
        );
        if (verbose) {
            std::cout << "Index: " << index << ", Hash: " << node.hashString() << std::endl;
        }
        return node;
    }

    MTNode node(globalTree);

    auto leftIndex = index << 1;
    auto rightIndex = leftIndex + 1;

    node.left = std::make_unique<MTNode>(
        globalHash(
            globalTree,
            localHashes,
            verbose,
            leftIndex,
            currLevelPartitionCount << 1,
            targetLevelPartitionCount
        ));
    node.right = std::make_unique<MTNode>(
        globalHash(
            globalTree,
            localHashes,
            verbose,
            rightIndex,
            currLevelPartitionCount << 1,
            targetLevelPartitionCount
        ));
    node.hashFromChildren();

    return node;
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "mpi-type-mismatch"
int main(int argc, char* argv[]) {
    int mpiSize, mpiRank;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);

    if (floorPot(mpiSize) != mpiSize) {
        if (mpiRank == 0) std::cerr << "Number of ranks must be a power of 2" << std::endl;
        return MPI_Finalize();
    }

    argparse::ArgumentParser program("mtsum", "1.0.3");
    program.set_usage_max_line_width(200);

    program.add_argument("-p")
        .help("number of processors to use")
        .metavar("processors")
        .default_value(8)
        .scan<'i', int>();

    program.add_argument("-a")
        .help("hashing algorithm to use, supported algorithms are md5, sha1, sha256, sha384, sha512")
        .metavar("algorithm")
        .default_value(std::string {"sha256"})
        .choices("md5", "sha1", "sha256", "sha384", "sha512");

    program.add_argument("-g")
        .help("output the merkle tree as DOT graph")
        .flag();

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
        if (mpiRank == 0) {
            std::cerr << err.what() << std::endl;
            std::cerr << program;
        }
        return MPI_Finalize();
    }

    auto p = program.get<int>("-p");
    auto algorithmName = program.get<std::string>("-a");
    auto filePath = program.get<std::string>("path");
    auto verbose = program.get<bool>("-v");
    auto benchmark = program.get<bool>("-b");
    auto graphOutput = program.get<bool>("-g");

    if (p < 1) {
        if (mpiRank == 0) std::cerr << "Number of processors must be at least 1" << std::endl;
        return MPI_Finalize();
    }

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);

    if (!file) {
        if (mpiRank == 0) std::cerr << "Error opening file: " << filePath << std::endl;
        return MPI_Finalize();
    }

    size_t fileSize = file.tellg();

    if (fileSize / mpiSize < MT_BLOCK_BALANCE_THRESHOLD) {
        if (mpiRank == 0) std::cerr << "File is too small!" << std::endl;
        return MPI_Finalize();
    }

    auto algorithm = EVP_get_digestbyname(algorithmName.c_str());

    if (!algorithm) {
        if (mpiRank == 0) std::cerr << "Invalid algorithm: " << algorithmName << std::endl;
        return MPI_Finalize();
    }

    std::vector<std::pair<size_t, size_t>> partitions;
    partition(partitions, 0, fileSize, 1, mpiSize);

    if (verbose && mpiRank == 0) {
        std::cout << "Algorithm: " << algorithmName << std::endl;
        std::cout << "Number of processors: " << p << std::endl;
        std::cout << "File size: " << fileSize << " bytes" << std::endl;
        std::cout << "Size per rank: " << partitions[0].second << " bytes" << std::endl;
        for (int i = 0; i < mpiSize; i++) {
            std::cout << "Rank " << i << ": offset=" << partitions[i].first << ", size=" << partitions[i].second
                      << std::endl;
        }
    }

    auto [offset, size] = static_cast<std::pair<ptrdiff_t, ptrdiff_t>>>(partitions[mpiRank]);
    std::vector<uint8_t> buffer(size);

    MPI_Barrier(MPI_COMM_WORLD);
    auto t0 = std::chrono::system_clock::now();

    // Open the file with MPI I/O collectively
    MPI_File fh;
    MPI_Status status;
    int ret = MPI_File_open(MPI_COMM_WORLD, filePath.c_str(), MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);

    if (ret != MPI_SUCCESS) {
        char error_string[MPI_MAX_ERROR_STRING];
        int length;
        MPI_Error_string(ret, error_string, &length);
        std::cerr << "Rank " << mpiRank << " - Error opening file: "<< error_string << std::endl;
        MPI_Finalize();
        return 1;
    }

    // Set the view for this process (optional for better performance)
    MPI_File_set_view(fh, offset, MPI_BYTE, MPI_BYTE, "native", MPI_INFO_NULL);

    // Collective read operation
    ret = MPI_File_read_all(fh, buffer.data(), size, MPI_BYTE, &status);

    if (ret != MPI_SUCCESS) {
        char error_string[MPI_MAX_ERROR_STRING];
        int length;
        MPI_Error_string(ret, error_string, &length);
        std::cerr << "Rank " << mpiRank << " - Error reading file: " << error_string << std::endl;
        MPI_Finalize();
        return 1;
    }

    // Close the file
    MPI_File_close(&fh);
    DoNotOptimize(buffer);

    MPI_Barrier(MPI_COMM_WORLD);
    auto t1 = std::chrono::system_clock::now();
    auto elapsed_par = std::chrono::duration<double>(t1 - t0);

    if ((verbose || benchmark) && mpiRank == 0) {
        std::cout << std::fixed << std::setprecision(2) << elapsed_par.count() << " s (";
        double gbPerSecond = (static_cast<double>(fileSize) / 1e9) / static_cast<double>(elapsed_par.count());
        std::cout << std::fixed << std::setprecision(2) << gbPerSecond << " GB/s)" << std::endl;
    }

    return MPI_Finalize();
}
#pragma clang diagnostic pop