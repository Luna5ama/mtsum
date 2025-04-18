#include <argparse/argparse.hpp>
#include "mtsum.hpp"

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("mtsum", "1.0.2");
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
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    auto p = program.get<int>("-p");
    auto algorithmName = program.get<std::string>("-a");
    auto filePath = program.get<std::string>("path");
    auto verbose = program.get<bool>("-v");
    auto benchmark = program.get<bool>("-b");
    auto graphOutput = program.get<bool>("-g");

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
            scope.tree.root = std::make_unique<MTNode>(buildTree(sbf, scope, 0, fileSize));
        }
    );
    allocTask.name("setup");
    rootTask.name("root");
    allocTask.precede(rootTask);
    executor.run(taskflow).wait();
    rootTask.name(tree.root->hashString());

    auto t1 = std::chrono::system_clock::now();
    auto elapsed_par = std::chrono::duration<double>(t1 - t0);

    if (graphOutput) {
        taskflow.dump(std::cout);
    } else {
        std::cout << rootTask.name() << std::endl;
    }

    if (verbose || benchmark) {
        std::cout << std::fixed << std::setprecision(2) << elapsed_par.count() << " s (";
        double gbPerSecond = (static_cast<double>(fileSize) / 1e9) / static_cast<double>(elapsed_par.count());
        std::cout << std::fixed << std::setprecision(2) << gbPerSecond << " GB/s)" << std::endl;
    }

    return 0;
}