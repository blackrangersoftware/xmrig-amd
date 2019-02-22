#include <string>
#include <sstream>
#include <mutex>
#include <cstring>
#include <thread>
#include "crypto/CryptoNight_monero.h"
#include "amd/OclCryptonightR_gen.h"
#include "amd/OclLib.h"
#include "amd/OclCache.h"
#include "amd/OclError.h"
#include "common/log/Log.h"

static std::string get_code(const V4_Instruction* code, int code_size)
{
    std::stringstream s;

	for (int i = 0; i < code_size; ++i)
    {
		const V4_Instruction inst = code[i];

		const uint32_t a = inst.dst_index;
		const uint32_t b = inst.src_index;

		switch (inst.opcode)
        {
		case MUL:
			s << 'r' << a << "*=r" << b << ';';
			break;

		case ADD:
            s << 'r' << a << "+=r" << b << '+' << inst.C << "U;";
			break;

		case SUB:
            s << 'r' << a << "-=r" << b << ';';
			break;

		case ROR:
        case ROL:
            s << 'r' << a << "=rotate(r" << a << ((inst.opcode == ROR) ? ",ROT_BITS-r" : ",r") << b << ");";
			break;

		case XOR:
			s << 'r' << a << "^=r" << b << ';';
			break;
		}

		s << '\n';
	}

    return s.str();
}

struct CacheEntry
{
    CacheEntry(xmrig::Variant variant, uint64_t height, size_t deviceIdx, std::string&& hash, cl_program program) :
        variant(variant),
        height(height),
        deviceIdx(deviceIdx),
        hash(std::move(hash)),
        program(program)
    {}

    xmrig::Variant variant;
    uint64_t height;
    size_t deviceIdx;
    std::string hash;
    cl_program program;
};

struct BackgroundTaskBase
{
    virtual ~BackgroundTaskBase() {}
    virtual void exec() = 0;
};

template<typename T>
struct BackgroundTask : public BackgroundTaskBase
{
    BackgroundTask(T&& func) : m_func(std::move(func)) {}
    void exec() override { m_func(); }

    T m_func;
};

static std::mutex CryptonightR_cache_mutex;
static std::mutex CryptonightR_build_mutex;
static std::vector<CacheEntry> CryptonightR_cache;

static std::mutex background_tasks_mutex;
static std::vector<BackgroundTaskBase*> background_tasks;
static std::thread* background_thread = nullptr;

static void background_thread_proc()
{
    std::vector<BackgroundTaskBase*> tasks;
    for (;;) {
        tasks.clear();
        {
            std::lock_guard<std::mutex> g(background_tasks_mutex);
            background_tasks.swap(tasks);
        }

        for (BackgroundTaskBase* task : tasks) {
            task->exec();
            delete task;
        }

        OclCache::sleep(500);
    }
}

template<typename T>
static void background_exec(T&& func)
{
    BackgroundTaskBase* task = new BackgroundTask<T>(std::move(func));

    std::lock_guard<std::mutex> g(background_tasks_mutex);
    background_tasks.push_back(task);
    if (!background_thread) {
        background_thread = new std::thread(background_thread_proc);
    }
}

void CryptonightR_release(GpuContext* ctx)
{
    OclLib::releaseProgram(ctx->ProgramCryptonightR);

    std::lock_guard<std::mutex> g(CryptonightR_cache_mutex);

    // Remove old programs from cache
    for (size_t i = 0; i < CryptonightR_cache.size();)
    {
        const CacheEntry& entry = CryptonightR_cache[i];
        if ((entry.deviceIdx == ctx->deviceIdx))
        {
            //LOG_INFO("CryptonightR: program for height %llu released (GpuContext release)", entry.height);
            CryptonightR_cache[i] = std::move(CryptonightR_cache.back());
            CryptonightR_cache.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

static cl_program CryptonightR_build_program(
    const GpuContext* ctx,
    xmrig::Variant variant,
    uint64_t height,
    cl_kernel old_kernel,
    std::string source,
    std::string options,
    std::string hash)
{
    if (old_kernel) {
        OclLib::releaseKernel(old_kernel);
    }

    std::vector<cl_program> old_programs;
    old_programs.reserve(32);
    {
        std::lock_guard<std::mutex> g(CryptonightR_cache_mutex);

        // Remove old programs from cache
        for (size_t i = 0; i < CryptonightR_cache.size();)
        {
            const CacheEntry& entry = CryptonightR_cache[i];
            if ((entry.variant == variant) && (entry.height + PRECOMPILATION_DEPTH < height))
            {
                //LOG_INFO("CryptonightR: program for height %llu released (old program)", entry.height);
                old_programs.push_back(entry.program);
                CryptonightR_cache[i] = std::move(CryptonightR_cache.back());
                CryptonightR_cache.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    for (cl_program p : old_programs) {
        OclLib::releaseProgram(p);
    }

    std::lock_guard<std::mutex> g1(CryptonightR_build_mutex);

    cl_program program = nullptr;
    {
        std::lock_guard<std::mutex> g(CryptonightR_cache_mutex);

        // Check if the cache already has this program (some other thread might have added it first)
        for (const CacheEntry& entry : CryptonightR_cache)
        {
            if ((entry.variant == variant) && (entry.height == height) && (entry.deviceIdx == ctx->deviceIdx) && (entry.hash == hash))
            {
                program = entry.program;
                break;
            }
        }
    }

    if (program) {
        return program;
    }

    cl_int ret;
    const char* s = source.c_str();
    program = OclLib::createProgramWithSource(ctx->opencl_ctx, 1, &s, nullptr, &ret);
    if (ret != CL_SUCCESS)
    {
        LOG_ERR("CryptonightR: clCreateProgramWithSource returned error %s", OclError::toString(ret));
        return nullptr;
    }

    ret = OclLib::buildProgram(program, 1, &ctx->DeviceID, options.c_str());
    if (ret != CL_SUCCESS)
    {
        OclLib::releaseProgram(program);
        LOG_ERR("CryptonightR: clBuildProgram returned error %s", OclError::toString(ret));
        return nullptr;
    }

    ret = OclCache::wait_build(program, ctx->DeviceID);
    if (ret != CL_SUCCESS)
    {
        OclLib::releaseProgram(program);
        LOG_ERR("CryptonightR: wait_build returned error %s", OclError::toString(ret));
        return nullptr;
    }

    //LOG_INFO("CryptonightR: program for height %llu compiled", height);

    {
        std::lock_guard<std::mutex> g(CryptonightR_cache_mutex);
        CryptonightR_cache.emplace_back(variant, height, ctx->deviceIdx, std::move(hash), program);
    }
    return program;
}

static bool is_64bit(xmrig::Variant variant)
{
    return false;
}

cl_program CryptonightR_get_program(GpuContext* ctx, xmrig::Variant variant, uint64_t height, bool background, cl_kernel old_kernel)
{
    if (background) {
        background_exec([=](){ CryptonightR_get_program(ctx, variant, height, false, old_kernel); });
        return nullptr;
    }

    const char* source_code_template =
        #include "opencl/wolf-aes.cl"
        #include "opencl/cryptonight_r.cl"
    ;
    const char include_name[] = "XMRIG_INCLUDE_RANDOM_MATH";
    const char* offset = strstr(source_code_template, include_name);
    if (!offset)
    {
        LOG_ERR("CryptonightR_get_program: XMRIG_INCLUDE_RANDOM_MATH not found in cryptonight_r.cl", variant);
        return nullptr;
    }

    V4_Instruction code[256];
    int code_size;
    switch (variant)
    {
    case xmrig::VARIANT_WOW:
        code_size = v4_random_math_init<xmrig::VARIANT_WOW>(code, height);
        break;
    case xmrig::VARIANT_4:
        code_size = v4_random_math_init<xmrig::VARIANT_4>(code, height);
        break;
    default:
        LOG_ERR("CryptonightR_get_program: invalid variant %d", variant);
        return nullptr;
    }

    std::string source_code(source_code_template, offset);
    source_code.append(get_code(code, code_size));
    source_code.append(offset + sizeof(include_name) - 1);

    char options[512] = {};
    OclCache::getOptions(xmrig::CRYPTONIGHT, variant, ctx, options, sizeof(options));

    char variant_buf[64];
    snprintf(variant_buf, sizeof(variant_buf), " -DVARIANT=%d", static_cast<int>(variant));
    strcat(options, variant_buf);

    if (is_64bit(variant))
    {
        strcat(options, " -DRANDOM_MATH_64_BIT");
    }

    const char* source = source_code.c_str();
    std::string hash;
    if (ctx->DeviceString.empty() && !OclCache::get_device_string(ctx->platformIdx, ctx->DeviceID, ctx->DeviceString)) {
        return nullptr;
    }
    OclCache::calc_hash(ctx->DeviceString, source, options, hash);

    {
        std::lock_guard<std::mutex> g(CryptonightR_cache_mutex);

        // Check if the cache has this program
        for (const CacheEntry& entry : CryptonightR_cache)
        {
            if ((entry.variant == variant) && (entry.height == height) && (entry.deviceIdx == ctx->deviceIdx) && (entry.hash == hash))
            {
                //LOG_INFO("CryptonightR: program for height %llu found in cache", height);
                return entry.program;
            }
        }
    }

    return CryptonightR_build_program(ctx, variant, height, old_kernel, source, options, hash);
}
