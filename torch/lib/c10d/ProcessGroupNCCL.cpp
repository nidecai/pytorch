#include "ProcessGroupNCCL.hpp"
#include "private/CUDAUtils.hpp"

#include <THC.h>

#include <map>
#include <unordered_set>

namespace c10d {

namespace {

// NCCL op mapping
std::map<ReduceOp, ncclRedOp_t> ncclOp = {
    {ReduceOp::MIN, ncclMin},
    {ReduceOp::MAX, ncclMax},
    {ReduceOp::SUM, ncclSum},
    {ReduceOp::PRODUCT, ncclProd},
};

// NCCL type typing
std::map<at::ScalarType, ncclDataType_t> ncclDataType = {
    {at::kChar, ncclInt8},
    {at::kByte, ncclUint8},
    {at::kFloat, ncclFloat},
    {at::kDouble, ncclDouble},
    {at::kInt, ncclInt32},
    {at::kLong, ncclInt64},
    {at::kHalf, ncclHalf},
};

// Helper function that gets the data type and issues error if not supported
ncclDataType_t getNcclDataType(at::ScalarType type) {
  try {
    return ncclDataType.at(type);
  } catch (std::out_of_range& e) {
    throw std::runtime_error("Unsupported data type for NCCL process group");
  }
}

// Get the deviceList String from the list of devices
std::string getKeyFromDevices(const std::vector<int>& devices) {
  std::string deviceList;
  for (auto device : devices) {
    if (deviceList.empty()) {
      deviceList = std::to_string(device);
    } else {
      deviceList += "," + std::to_string(device);
    }
  }
  return deviceList;
}

// Get the list of devices from list of tensors
std::vector<int> getDevicesOfTensors(const std::vector<at::Tensor>& tensors) {
  std::vector<int> res;
  for (auto& tensor : tensors) {
    res.push_back(tensor.get_device());
  }
  return res;
}

// Helper that lets the input ncclStreams to wait for the THC stream
void syncStreams(
    THCState* thcState,
    const std::vector<int>& devices,
    std::vector<CUDAEvent>& ncclEvents,
    std::vector<CUDAStream>& ncclStreams) {
  CUDADevice gpuGuard;
  for (size_t i = 0; i < devices.size(); ++i) {
    gpuGuard.setDevice(devices[i]);
    auto currentThcStream =
        THCState_getCurrentStreamOnDevice(thcState, devices[i]);
    CUDAStream& ncclStream = ncclStreams[i];
    CUDAEvent& ncclEvent = ncclEvents[i];

    C10D_CUDA_CHECK(cudaEventRecord(ncclEvent.getEvent(), currentThcStream));
    C10D_CUDA_CHECK(
        cudaStreamWaitEvent(ncclStream.getStream(), ncclEvent.getEvent(), 0));
  }
}

} // namespace

ProcessGroupNCCL::WorkNCCL::WorkNCCL(const std::vector<int>& devices)
    : devices_(devices) {
  CUDADevice gpuGuard;
  cudaEvents_.resize(devices.size());
  // Now create the CUDA events
  for (size_t i = 0; i < devices.size(); ++i) {
    gpuGuard.setDevice(devices[i]);
    cudaEvents_[i] = CUDAEvent::create(cudaEventDisableTiming);
  }
}

ProcessGroupNCCL::WorkNCCL::~WorkNCCL() {}

// Check if the NCCL kernels are queued on the GPUs
bool ProcessGroupNCCL::WorkNCCL::isCompleted() const {
  return true;
}

// Helper that checks if the NCCL kernels are completed on the GPUs
bool ProcessGroupNCCL::WorkNCCL::finishedGPUExecution() const {
  CUDADevice gpuGuard;
  for (size_t i = 0; i < devices_.size(); ++i) {
    gpuGuard.setDevice(devices_[i]);
    auto& cudaEvent = cudaEvents_[i];
    // Checking the work's corresponding CUDA events' status
    auto ret = cudaEventQuery(cudaEvent.getEvent());
    if (ret != cudaSuccess && ret != cudaErrorNotReady) {
      C10D_CUDA_CHECK(ret);
    }
    if (ret == cudaErrorNotReady) {
      return false;
    }
  }
  return true;
}

// Same as synchronize(), and will always return true
bool ProcessGroupNCCL::WorkNCCL::wait() {
  synchronize();
  return true;
}

// Waiting on the work's corresponding CUDA events
void ProcessGroupNCCL::WorkNCCL::synchronize() {
  auto thcState = ::at::globalContext().lazyInitCUDA();
  CUDADevice gpuGuard;
  for (size_t i = 0; i < devices_.size(); ++i) {
    gpuGuard.setDevice(devices_[i]);
    auto thcStream = THCState_getCurrentStreamOnDevice(thcState, devices_[i]);
    auto& cudaEvent = cudaEvents_[i];
    // Let THC stream wait for the NCCL stream
    C10D_CUDA_CHECK(cudaStreamWaitEvent(thcStream, cudaEvent.getEvent(), 0));
  }
}

bool ProcessGroupNCCL::WorkNCCL::isSuccess() const {
  return true;
}

const std::exception& ProcessGroupNCCL::WorkNCCL::exception() const {
  throw std::runtime_error(
      "exception() is not supported by NCCL process "
      "group's work, since isSuccess() will always return true, and "
      "isCompleted() and wait() will either succeed or throw");
}

std::unordered_map<ssize_t, ssize_t> ProcessGroupNCCL::pgUniqueNCCLIDCnt_;
ssize_t ProcessGroupNCCL::processGroupCounter_ = -1;
std::mutex ProcessGroupNCCL::pgTrackingLock_;

ProcessGroupNCCL::ProcessGroupNCCL(
    const std::shared_ptr<Store>& store,
    int rank,
    int size)
    : ProcessGroup(rank, size), store_(store) {
  thcState_ = ::at::globalContext().lazyInitCUDA();
  // Generate the Process Group ID for current PG, this needs to be identical
  // for all processes
  std::unique_lock<std::mutex> lock(pgTrackingLock_);
  ++processGroupCounter_;
  pgUniqueNCCLIDCnt_[processGroupCounter_] = -1;
  processGroupID_ = std::to_string(processGroupCounter_);
}

ProcessGroupNCCL::~ProcessGroupNCCL() {
  std::unique_lock<std::mutex> lock(pgTrackingLock_);
  pgUniqueNCCLIDCnt_.erase(std::stoull(processGroupID_));
}

void ProcessGroupNCCL::broadcastUniqueNCCLID(ncclUniqueId* ncclID) {
  // Every time when we create a new unique NCCL ID, we need to use a new
  // global key to access/update the store.
  // The key is a combination of processGroupID_ and the current count of
  // NCCL unique ID created
  std::unique_lock<std::mutex> lock(pgTrackingLock_);
  auto processGroupIDKey = std::stoull(processGroupID_);
  auto uniqueNCCLIDCnt = pgUniqueNCCLIDCnt_[processGroupIDKey] + 1;
  pgUniqueNCCLIDCnt_[processGroupIDKey] = uniqueNCCLIDCnt;

  lock.unlock();

  std::string storeKey =
      processGroupID_ + "_" + std::to_string(uniqueNCCLIDCnt);

  // Rank 0 writes to the store as bcast
  if (rank_ == 0) {
    auto ncclIDVal = std::vector<uint8_t>(
        reinterpret_cast<uint8_t*>(ncclID),
        reinterpret_cast<uint8_t*>(ncclID) + NCCL_UNIQUE_ID_BYTES);
    store_->set(storeKey, ncclIDVal);
    // Other ranks get to the store
  } else {
    auto ncclIDVal = store_->get(storeKey);
    // Just a sanity check
    if (ncclIDVal.size() != NCCL_UNIQUE_ID_BYTES) {
      throw std::runtime_error(
          "Unexpected NCCL unique ID length received "
          "from the store");
    }
    // Now put the data back to the input pointer
    memcpy(ncclID, ncclIDVal.data(), NCCL_UNIQUE_ID_BYTES);
  }
}

std::vector<std::shared_ptr<NCCLComm>>& ProcessGroupNCCL::getNCCLComm(
    const std::string& devicesKey,
    const std::vector<int>& devices) {
  // Sanity check
  if (devicesKey.empty()) {
    throw std::runtime_error(
        "Not able to create/get the NCCL Communicator since "
        "the GPU devices are not known");
  }
  if (devNCCLCommMap_.find(devicesKey) != devNCCLCommMap_.end()) {
    // Reuse the cached communicator if there is one.
    return devNCCLCommMap_[devicesKey];
  }
  // NCCL communicator not cached, create a new entry
  std::vector<std::shared_ptr<NCCLComm>> ncclComms;
  ncclComms.resize(devices.size());

  // Create the unique NCCL ID and broadcast it
  ncclUniqueId ncclID;

  if (rank_ == 0) {
    C10D_NCCL_CHECK(ncclGetUniqueId(&ncclID));
  }

  // Broadcast so that each process can have a unique NCCL ID
  broadcastUniqueNCCLID(&ncclID);

  CUDADevice gpuGuard;

  std::vector<CUDAEvent> eventVal;
  std::vector<CUDAStream> streamVal;

  eventVal.resize(devices.size());
  streamVal.resize(devices.size());

  // Create the NCCL communicators for each GPU
  C10D_NCCL_CHECK(ncclGroupStart());

  for (size_t i = 0; i < devices.size(); ++i) {
    // GPU world size and GPU rank
    int numRanks = getSize() * devices.size();
    int rank = getRank() * devices.size() + i;

    gpuGuard.setDevice(devices[i]);
    ncclComms[i] = NCCLComm::create(numRanks, rank, ncclID);

    // Also create the NCCL streams and events
    streamVal[i] = CUDAStream::create();
    // Event created using cudaEventDisableTiming flag and not
    // cudaEventBlockingSync flag will provide the best performance when used
    // with cudaStreamWaitEvent() and cudaEventQuery(). Since we here don't
    // measure the performance using cudaEvent, this should be set.
    eventVal[i] = CUDAEvent::create(cudaEventDisableTiming);
  }

  C10D_NCCL_CHECK(ncclGroupEnd());

  // Move the NCCL resource to cache
  devNCCLCommMap_.emplace(devicesKey, std::move(ncclComms));
  ncclStreams_.emplace(devicesKey, std::move(streamVal));
  ncclEvents_.emplace(devicesKey, std::move(eventVal));

  return devNCCLCommMap_[devicesKey];
}

// Helper function that checks the input and output tensors for validity
void ProcessGroupNCCL::tensorCheckHelper(
    const std::vector<at::Tensor>& input,
    const std::vector<at::Tensor>& output,
    int outputOverInput) {
  if (input.size() != output.size()) {
    throw std::runtime_error(
        "Input tensor sequence should have the same "
        "number of tensors as the output tensor sequence");
  }

  if (input.size() == 0) {
    throw std::runtime_error("The number of input tensors should not be zero");
  }

  if (input.size() > static_cast<size_t>(thcState_->numDevices)) {
    throw std::runtime_error(
        "The number of input tensors is larger than "
        "the number of available GPUs");
  }

  // To make sure each tensor is on separate devices
  std::unordered_set<int> usedDevices;
  usedDevices.reserve(input.size());

  auto inputNumElement = input[0].numel();
  auto elementType = input[0].type().scalarType();

  for (size_t i = 0; i < input.size(); ++i) {
    //  Check to make sure it's a GPU dense tensor
    if (!(input[i].type().is_cuda() && !input[i].type().is_sparse() &&
          output[i].type().is_cuda() && !output[i].type().is_sparse())) {
      throw std::runtime_error(
          "Only CUDA dense tensor is supported for NCCL "
          "collective operations");
    }
    // Check the tensor type is identical
    if (input[i].type().scalarType() != elementType ||
        output[i].type().scalarType() != elementType) {
      throw std::runtime_error(
          "Expecting all GPU tensors to have identical "
          "type");
    }
    // Check the input tensor size is identical
    if (input[i].numel() != inputNumElement) {
      throw std::runtime_error(
          "Expecting all input tensors to have identical "
          "number of elements");
    }
    // Check the output tensor size equals to input tensor size
    if (output[i].numel() != inputNumElement * outputOverInput) {
      throw std::runtime_error(
          "The number of elements of output tensor does "
          "not match the number of elements of the input "
          "tensor");
    }
    // Contiguous verification
    if (!input[i].is_contiguous() || !output[i].is_contiguous()) {
      throw std::runtime_error("Expecting all GPU tensors to be contiguous");
    }

    bool inserted;
    std::tie(std::ignore, inserted) = usedDevices.insert(input[i].get_device());
    // Device verification, if the insertion didn't take place
    if (!inserted) {
      throw std::runtime_error("Expecting inputs on different GPU devices");
    }

    // Now check the output device
    if (input[i].get_device() != output[i].get_device()) {
      throw std::runtime_error(
          "Expecting input and output tensors to be on "
          "the same device");
    }
  }
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::allreduce(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts) {
  tensorCheckHelper(tensors, tensors);

  auto devices = getDevicesOfTensors(tensors);
  auto key = getKeyFromDevices(devices);
  auto& ncclComms = getNCCLComm(key, devices);

  // First let NCCL streams wait for THC stream
  syncStreams(thcState_, devices, ncclEvents_[key], ncclStreams_[key]);

  // Work itself will create the CUDA events on all GPUs of tensors
  auto work = std::make_shared<ProcessGroupNCCL::WorkNCCL>(devices);

  CUDADevice gpuGuard;

  std::unique_lock<std::mutex> cudaFreeMutexLock(
      *(THCCachingAllocator_getCudaFreeMutex()));

  C10D_NCCL_CHECK(ncclGroupStart());

  for (size_t i = 0; i < tensors.size(); ++i) {
    gpuGuard.setDevice(devices[i]);
    CUDAStream& ncclStream = ncclStreams_[key][i];

    C10D_NCCL_CHECK(ncclAllReduce(
        tensors[i].data_ptr(),
        tensors[i].data_ptr(),
        tensors[i].numel(),
        getNcclDataType(tensors[i].type().scalarType()),
        ncclOp[opts.reduceOp],
        ncclComms[i]->getNcclComm(),
        ncclStream.getStream()));
  }

  C10D_NCCL_CHECK(ncclGroupEnd());

  // Event should only be recorded after the ncclGroupEnd()
  for (size_t i = 0; i < tensors.size(); ++i) {
    CUDAStream& ncclStream = ncclStreams_[key][i];
    CUDAEvent& cudaEvent = work->cudaEvents_[i];

    C10D_CUDA_CHECK(
        cudaEventRecord(cudaEvent.getEvent(), ncclStream.getStream()));
  }

  return work;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupNCCL::broadcast(
    std::vector<at::Tensor>& tensors,
    const BroadcastOptions& opts) {
  tensorCheckHelper(tensors, tensors);

  auto devices = getDevicesOfTensors(tensors);
  auto key = getKeyFromDevices(devices);
  auto& ncclComms = getNCCLComm(key, devices);

  // First let NCCL streams wait for THC stream
  syncStreams(thcState_, devices, ncclEvents_[key], ncclStreams_[key]);

  // Work itself will create the CUDA events on all GPUs of tensors
  auto work = std::make_shared<ProcessGroupNCCL::WorkNCCL>(devices);

  CUDADevice gpuGuard;

  std::unique_lock<std::mutex> cudaFreeMutexLock(
      *(THCCachingAllocator_getCudaFreeMutex()));

  C10D_NCCL_CHECK(ncclGroupStart());

  for (size_t i = 0; i < tensors.size(); ++i) {
    gpuGuard.setDevice(devices[i]);
    CUDAStream& ncclStream = ncclStreams_[key][i];
    // root rank of the the GPU
    int root = opts.rootRank * tensors.size() + opts.rootTensor;

    C10D_NCCL_CHECK(ncclBcast(
        tensors[i].data_ptr(),
        tensors[i].numel(),
        getNcclDataType(tensors[i].type().scalarType()),
        root,
        ncclComms[i]->getNcclComm(),
        ncclStream.getStream()));
  }

  C10D_NCCL_CHECK(ncclGroupEnd());

  // Event should only be recorded after the ncclGroupEnd()
  for (size_t i = 0; i < tensors.size(); ++i) {
    CUDAStream& ncclStream = ncclStreams_[key][i];
    CUDAEvent& cudaEvent = work->cudaEvents_[i];

    C10D_CUDA_CHECK(
        cudaEventRecord(cudaEvent.getEvent(), ncclStream.getStream()));
  }

  return work;
}

} // namespace c10d
