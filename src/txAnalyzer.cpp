#ifndef __TX_ANALYZER__H
#define __TX_ANALYZER__H

#include "txAnalyzer.h"
#include "uint256.h"
#include "primitives/transaction.h"
#include "validation.h"
#include "chainparams.h"
#include "rpc/server.h"
#include <index/txindex.h>
#include <exception>
#include <shutdown.h>
#include <boost/thread.hpp>
#include <boost/asio/signal_set.hpp>
#include <cassert>
#include <init.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <rpc/util.h>


struct txinfo {
    CAmount fee;
    unsigned int size;
    std::vector<CTxIn> parents;
    txinfo(CAmount f, unsigned int s, std::vector<CTxIn> p) : fee(f), size(s), parents(p) {}
};

// function prototypes
CAmount getTXFee(std::string txHash);
std::pair<int, unsigned int> getTXSize(std::string txHash);
std::pair<int, std::vector<CTxIn>> getTXParents(std::string txHash);
void txAnalyzer(std::pair<std::string, FILE *> f);
void WriteToDisk(std::pair<std::unordered_map<std::string, txinfo *>, std::unordered_set<std::string>> tinfo, std::string f);

// static variables
static std::vector<std::pair<std::string, FILE *>> fHandles;
static bool InitSuccess = true;
static fs::path basedir;

// initialize tx analyzer
bool initTXAnalyzer(std::string ifname)
{
    basedir = GetDataDir() / "tx-analysis-files";
    fs::path inputfilepath =  basedir / ifname;

    if(!fs::exists(inputfilepath))
    {
        std::cout << "> ERROR - input path: <" << inputfilepath.string() << "> doesn't exist" << std::endl;
        return false;
    }
    if(!fs::is_regular_file(inputfilepath))
    {
        std::cout << "> ERROR - input path: <" << inputfilepath.string() << "> is not a regular file" << std::endl;
        return false;
    }

    FILE * inputfilehandle = fsbridge::fopen(inputfilepath, "r");
    if(!inputfilehandle)
    {
        std::cout << "> ERROR - couldn't open file <" << inputfilepath.string() << ">" << std::endl;
        return false;
    }

    char * line = NULL;
    size_t len = 0;
    ssize_t nread;

    while ((nread = getline(&line, &len, inputfilehandle)) != -1) {
        if(line[nread - 1] == '\n')
            line[nread - 1] = 0;
        fs::path fPath = basedir / line;
        if(!fs::exists(fPath))
        {
            std::cout << "> WARN - file path: <" << fPath.string() << "> doesn't exist" << std::endl;
            InitSuccess = false;
        }
        else if(!fs::is_regular_file(fPath))
        {
            std::cout << "> WARN - file path: <" << fPath.string() << "> is not a regular file" << std::endl;
            InitSuccess = false;
        }
        else
        {
            FILE * fHandle = fsbridge::fopen(fPath, "r");
            if(!fHandle)
            {
                std::cout << "> WARN - couldn't open file <" << fPath.string() << ">" << std::endl;
                InitSuccess = false;
            }
            else
            {
                fHandles.push_back({line, fHandle});
                std::cout << "> INFO - file <" << fPath.string() << "> added to processing queue" << std::endl;
            }
        }
    }
    fclose(inputfilehandle);
    if (line)
        free(line);

    return true;
}

// tx analyzer thread does not interfere with the main thread
void txAnalyzerThread()
{
    char opt = ' ';
    if(!InitSuccess)
    {
        std::cout << "> some files failed to be opened successfully. continue? [y/n]" << std::endl;
        std::cin >> opt;
        while(!(opt == 'y' || opt == 'Y' || opt == 'n' || opt == 'N'))
        {
            std::cout << "> invalid character entered. continue? [y/n]" << std::endl;
            std::cin >> opt;
        }
        if(opt == 'n' || opt == 'N')
        {
            std::cout << "> TX analysis complete" << std::endl;
            return;
        }
    }

    for(unsigned int i = 0; i < fHandles.size() && !ShutdownRequested(); i++)
    {
        txAnalyzer(fHandles.at(i));
    }

    std::cout << "> TX analysis complete" << std::endl;
}

// read transaction hashes to analyze from a file
bool ReadTXHashesFromFile(FILE * fHandle, std::vector<std::string> & hashes)
{
    char * line = NULL;
    size_t len = 0;
    ssize_t nread;

    while ((nread = getline(&line, &len, fHandle)) != -1 && !ShutdownRequested())
    {
        if(line[nread - 1] == '\n')
            line[nread - 1] = 0;

        std::string hash = std::string(line);
        if(hash.size() > 64)
        {
            std::cout << "> ERROR : invalid tx hash <" << hash << std::endl;
            return false;
        }
        hashes.push_back(hash);
    }
    fclose(fHandle);
    return true;
}

// analyze transactions: for each transaction, its fee, size, and parents are found and written to file
void txAnalyzer(std::pair<std::string, FILE *> f)
{
    std::pair<std::unordered_map<std::string, txinfo *>, std::unordered_set<std::string>> _txinfo;
    std::vector<std::string> hashes;
    std::vector<std::string> exceptions;
    std::cout << ">> INFO - Reading hashes from file: <" << f.first << ">" << std::endl;
    if(!ReadTXHashesFromFile(f.second, hashes))
    {
        std::cerr << ">> ERROR : Unable to read file <" << f.first << ">" << std::endl;
        return;
    }
    std::cout << ">> INFO - Successfully read hashes from file: <" << f.first << ">" << std::endl;
    std::cout << ">> INFO - Beginning transaction analysis..." << std::endl;

    std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();
    unsigned int ctr = 0;
    for(auto hash : hashes)
    {
        if(ShutdownRequested())
            break;
        auto fee = getTXFee(hash);
        auto size = getTXSize(hash);
        auto parents = getTXParents(hash);
        if(fee == -2 || size.first == -2 || parents.first == -2)
        {
            _txinfo.second.insert(hash);
        }
        else if(fee >= 0 && size.first >=0 && parents.first >= 0)
        {
            _txinfo.first[hash] = new txinfo(fee, size.second, parents.second);
        }
        else
        {
            exceptions.push_back(hash);
        }
        ++ctr;
        std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        auto ms = milliseconds.count() % 1000;
        auto secs = (milliseconds.count() / 1000) % 60;
        auto mins = (milliseconds.count() / 1000 / 60) % 60;
        auto hrs  = ((milliseconds.count() / 1000 / 60) / 60) % 24;
        auto days = ((milliseconds.count() / 1000 / 60) / 60) / 24;
        char timestr[20];
        if(days > 0)
            sprintf(timestr, "%ld:%02ld:%02ld:%02ld", days, hrs, mins, secs);
        else
            sprintf(timestr, "%02ld:%02ld:%02ld", hrs, mins, secs);
        std::cout << "Progress:\t" << ctr << "/" << hashes.size() << " (" << std::fixed << std::setprecision(4) << (ctr * 1.0 / hashes.size() * 100) << "%) [elapsed "
        << timestr << "." << std::fixed << std::setprecision(3) << ms << "]\r" << std::flush;
    }
    std::cout << std::endl;
    std::cout << "UKN: " << _txinfo.second.size() << std::endl;
    std::cout << "EXC: " << exceptions.size() << std::endl;

    WriteToDisk(_txinfo, f.first);
}

// find parents of a given transaction
std::pair<int, std::vector<CTxIn>> getTXParents(std::string txHash)
{
    CBlockIndex* blockindex = nullptr;
    bool f_txindex_ready = false;
    if (g_txindex && !blockindex) {
        f_txindex_ready = g_txindex->BlockUntilSyncedToCurrentChain();
    }
    CTransactionRef ptx;
    uint256 hash_block;
    uint256 hash = ParseHashV(txHash, "parameter 1");
    try {
        if (!GetTransaction(hash, ptx, Params().GetConsensus(), hash_block, blockindex)) {
            std::string errmsg;
            if (blockindex) {
                if (!(blockindex->nStatus & BLOCK_HAVE_DATA)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
                }
                errmsg = "No such transaction found in the provided block";
            } else if (!g_txindex) {
                errmsg = "No such mempool transaction. Use -txindex to enable blockchain transaction queries";
            } else if (!f_txindex_ready) {
                errmsg = "No such mempool transaction. Blockchain transactions are still in the process of being indexed";
            } else {
                errmsg = "No such mempool or blockchain transaction";
            }
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errmsg + ". Use gettransaction for wallet transactions.");
        }
        CTransaction tx = *ptx;
        return std::make_pair(0, tx.vin);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::vector<CTxIn> v;
        return std::make_pair(-1, v);
    }
    catch (...) {
        std::vector<CTxIn> v;
        return std::make_pair(-2, v);
    }
}

// find size of a given transaction
std::pair<int, unsigned int> getTXSize(std::string txHash)
{
    CBlockIndex* blockindex = nullptr;
    bool f_txindex_ready = false;
    if (g_txindex && !blockindex) {
        f_txindex_ready = g_txindex->BlockUntilSyncedToCurrentChain();
    }
    CTransactionRef ptx;
    uint256 hash_block;
    uint256 hash = ParseHashV(txHash, "parameter 1");
    try {
        if (!GetTransaction(hash, ptx, Params().GetConsensus(), hash_block, blockindex)) {
            std::string errmsg;
            if (blockindex) {
                if (!(blockindex->nStatus & BLOCK_HAVE_DATA)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
                }
                errmsg = "No such transaction found in the provided block";
            } else if (!g_txindex) {
                errmsg = "No such mempool transaction. Use -txindex to enable blockchain transaction queries";
            } else if (!f_txindex_ready) {
                errmsg = "No such mempool transaction. Blockchain transactions are still in the process of being indexed";
            } else {
                errmsg = "No such mempool or blockchain transaction";
            }
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errmsg + ". Use gettransaction for wallet transactions.");
        }
        CTransaction tx = *ptx;
        return std::make_pair(0, tx.GetTotalSize());
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return std::make_pair(-1, 0);
    }
    catch (...) {
        return std::make_pair(-2, 0);
    }
}

// find fee of a given transaction
CAmount getTXFee(std::string txHash)
{
    CBlockIndex* blockindex = nullptr;
    bool f_txindex_ready = false;
    if (g_txindex && !blockindex) {
        f_txindex_ready = g_txindex->BlockUntilSyncedToCurrentChain();
    }
    CTransactionRef ptx;
    uint256 hash_block;
    uint256 hash = ParseHashV(txHash, "parameter 1");
    try {
        if (!GetTransaction(hash, ptx, Params().GetConsensus(), hash_block, blockindex)) {
            std::string errmsg;
            if (blockindex) {
                if (!(blockindex->nStatus & BLOCK_HAVE_DATA)) {
                    throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
                }
                errmsg = "No such transaction found in the provided block";
            } else if (!g_txindex) {
                errmsg = "No such mempool transaction. Use -txindex to enable blockchain transaction queries";
            } else if (!f_txindex_ready) {
                errmsg = "No such mempool transaction. Blockchain transactions are still in the process of being indexed";
            } else {
                errmsg = "No such mempool or blockchain transaction";
            }
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errmsg + ". Use gettransaction for wallet transactions.");
        }
        CTransaction tx = *ptx;
        CAmount inVal = 0;
        for (auto vin : tx.vin)
        {
            CBlockIndex* blockindex = nullptr;
            bool f_txindex_ready = false;
            if (g_txindex && !blockindex) {
                f_txindex_ready = g_txindex->BlockUntilSyncedToCurrentChain();
            }
            CTransactionRef ptx;
            uint256 hash_block;
            uint256 hash = ParseHashV(vin.prevout.hash.ToString(), "parameter 1");
            if (!GetTransaction(hash, ptx, Params().GetConsensus(), hash_block, blockindex)) {
                std::string errmsg;
                if (blockindex) {
                    if (!(blockindex->nStatus & BLOCK_HAVE_DATA)) {
                        throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
                    }
                    errmsg = "No such transaction found in the provided block";
                } else if (!g_txindex) {
                    errmsg = "No such mempool transaction. Use -txindex to enable blockchain transaction queries";
                } else if (!f_txindex_ready) {
                    errmsg = "No such mempool transaction. Blockchain transactions are still in the process of being indexed";
                } else {
                    errmsg = "No such mempool or blockchain transaction";
                }
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errmsg + ". Use gettransaction for wallet transactions.");
            }
            inVal += (*ptx).vout[vin.prevout.n].nValue;
        }
        CAmount outVal = 0;
        for (auto vout : tx.vout) {
            outVal += vout.nValue;
        }
        return inVal - outVal;
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return CAmount(-1);
    }
    catch (...) {
        return CAmount(-2);
    }
}

// write data from analysis to file
void WriteToDisk(std::pair<std::unordered_map<std::string, txinfo *>, std::unordered_set<std::string>> tinfo, std::string f)
{
    std::string outfilename = f + "_out";
    std::string uknfilename = f + "_unknown";
    fs::path parentdirpath = basedir / (f + "_parents");

    if(!fs::exists(parentdirpath))
    {
        fs::create_directory(parentdirpath);
    }

    std::cout << ">> INFO - writing transaction fees and sizes to file: <" << outfilename << ">" << std::endl;
    std::cout << ">> INFO - writing transaction parents to directory: <" << (f + "_parents") << ">" << std::endl;
    fs::path outfilePath = basedir / outfilename;
    FILE * outfileHandle = fsbridge::fopen(outfilePath, "w");

    for(auto m : tinfo.first)
    {
        std::string out = m.first + ", " + std::to_string(m.second->fee) + ", " + std::to_string(m.second->size);
        fprintf(outfileHandle, "%s\n", out.c_str());
        fs::path parentfilePath = parentdirpath / m.first;
        FILE * parentfileHandle = fsbridge::fopen(parentfilePath, "w");
        for(auto p : m.second->parents)
        {
            fprintf(parentfileHandle, "%s\n", p.prevout.hash.ToString().c_str());
        }
        fclose(parentfileHandle);
    }
    fclose(outfileHandle);

    std::cout << ">> INFO - writing hashes of unknown transactions to file: <" << uknfilename << ">" << std::endl;
    fs::path uknfilePath = basedir / uknfilename;
    FILE * uknfileHandle = fsbridge::fopen(uknfilePath, "w");

    for(auto h : tinfo.second)
    {
        fprintf(uknfileHandle, "%s\n", h.c_str());
    }
    fclose(uknfileHandle);
}

#endif /* __TX_ANALYZER__H */
