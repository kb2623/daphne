/*
 * Copyright 2021 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MTWrapper.h"

template<typename VT>
void MTWrapper<DenseMatrix<VT>>::executeSingleQueue(
            std::vector<std::function<typename MTWrapper<DenseMatrix<VT>>::PipelineFunc>> funcs, DenseMatrix<VT> ***res,
            Structure **inputs, size_t numInputs, size_t numOutputs, int64_t *outRows, int64_t *outCols,
            VectorSplit *splits, VectorCombine *combines, DCTX(ctx), bool verbose) {
    auto inputProps = this->getInputProperties(inputs, numInputs, splits);
    auto len = inputProps.first;
    auto mem_required = inputProps.second;
    mem_required += this->allocateOutput(res, numOutputs, outRows, outCols, combines);
    auto row_mem = mem_required / len;
    
    // create task queue (w/o size-based blocking)
    std::unique_ptr<TaskQueue> q = std::make_unique<BlockingTaskQueue>(len);

    auto batchSize8M = std::max(100ul, static_cast<size_t>(std::ceil(8388608 / row_mem)));
    this->initCPPWorkers(q.get(), batchSize8M, verbose);

#ifdef USE_CUDA
    if(this->_numCUDAThreads) {
        auto batchSize32M = std::max(100ul, static_cast<size_t>(std::ceil(33554432 / row_mem)));
        this->initCUDAWorkers(q.get(), batchSize32M, verbose);
        this->cudaPrefetchInputs(inputs, numInputs, mem_required, splits);
#ifndef NDEBUG
        std::cout << "Required memory (ins/outs): " << mem_required << "\nRequired mem/row: " << row_mem << std::endl;
        std::cout << "batchsizeCPU=" << batchSize8M << " batchsizeGPU=" << batchSize32M << std::endl;
#endif
    }
#endif

    // lock for aggregation combine
    // TODO: multiple locks per output
    std::mutex resLock;

    // create tasks and close input
    uint64_t startChunk = 0;
    uint64_t endChunk = 0;
    auto chunkParam = 1;
    LoadPartitioning lp(STATIC, len, chunkParam, this->_numThreads, false);
    while (lp.hasNextChunk()) {
        endChunk += lp.getNextChunk();
        q->enqueueTask(new CompiledPipelineTask<DenseMatrix<VT>>(CompiledPipelineTaskData<DenseMatrix<VT>>{funcs,
                inputs, numInputs, numOutputs, outRows, outCols, splits, combines, startChunk, endChunk, outRows,
                outCols, 0, ctx}, resLock, res));
        startChunk = endChunk;
    }
    q->closeInput();

    this->joinAll();
}

template<typename VT>
void MTWrapper<DenseMatrix<VT>>::executeQueuePerDeviceType(
        std::vector<std::function<typename MTWrapper<DenseMatrix<VT>>::PipelineFunc>> funcs, DenseMatrix<VT> ***res,
        Structure **inputs, size_t numInputs, size_t numOutputs, int64_t *outRows, int64_t *outCols,
        VectorSplit *splits, VectorCombine *combines, DCTX(ctx), bool verbose) {

    auto inputProps = this->getInputProperties(inputs, numInputs, splits);
    auto len = inputProps.first;
    auto mem_required = inputProps.second;
    mem_required += this->allocateOutput(res, numOutputs, outRows, outCols, combines);
    auto row_mem = mem_required / len;

    // ToDo: multi-device support :-P
//    auto cctx = ctx->getCUDAContext(0);
    float taskRatioCUDA = 0.5f;
//    auto buffer_usage = static_cast<float>(mem_required) / static_cast<float>(cctx->getMemBudget());

    auto gpu_task_len = static_cast<size_t>(std::ceil(static_cast<float>(len) * taskRatioCUDA));
    auto cpu_task_len = len - gpu_task_len;

    // create task queue (w/o size-based blocking)
    std::unique_ptr<TaskQueue> q_cpp = std::make_unique<BlockingTaskQueue>(cpu_task_len);
    auto batchSize8M = std::max(100ul, static_cast<size_t>(std::ceil(8388608 / row_mem)));
    this->initCPPWorkers(q_cpp.get(), batchSize8M, verbose);
    std::unique_ptr<TaskQueue> q_cuda = std::make_unique<BlockingTaskQueue>(gpu_task_len);
    auto batchSize32M = std::max(100ul, static_cast<size_t>(std::ceil(33554432 / row_mem)));
    this->initCUDAWorkers(q_cuda.get(), batchSize32M, verbose);

    // lock for aggregation combine
    // TODO: multiple locks per output
    std::mutex resLock;
//    auto rc1 = new DenseMatrix<VT>*[numOutputs];
//    DenseMatrix<VT>*** res_cuda = &rc1;
    DenseMatrix<VT>*** res_cuda = new DenseMatrix<VT>**[numOutputs];
    //    auto blksize = static_cast<size_t>(std::floor(cctx->getMemBudget() / row_mem));
    auto blksize = gpu_task_len / ctx->cuda_contexts.size();
#ifndef NDEBUG
    std::cout << "gpu_task_len:  " << gpu_task_len << "\ntaskRatioCUDA: " << taskRatioCUDA << "\nBlock size: "
              << blksize << std::endl;
#endif
    for (size_t i = 0; i < numOutputs; ++i) {
        res_cuda[i] = new DenseMatrix<VT>*;
        if(combines[i] == mlir::daphne::VectorCombine::ROWS) {
            auto rc2 = static_cast<DenseMatrix<VT> *>((*res[i]))->slice(0, gpu_task_len);
            (*res_cuda[i]) = rc2;
        }
        else if(combines[i] == mlir::daphne::VectorCombine::COLS) {
            (*res_cuda[i]) = static_cast<DenseMatrix<VT> *>((*res[i]))->slice(0, outRows[i], 0, gpu_task_len);
        }
        else {
            (*res_cuda[i]) = (*res[i]);
        }
    }

    for (uint32_t k = 0; k < gpu_task_len; k += blksize) {
        q_cuda->enqueueTask(new CompiledPipelineTaskCUDA<DenseMatrix<VT>>(CompiledPipelineTaskData<DenseMatrix<VT>>{funcs, inputs,
                numInputs, numOutputs, outRows, outCols, splits, combines, k, std::min(k + blksize, len), outRows,
                outCols, 0, ctx}, resLock, res_cuda));
    }
    q_cuda->closeInput();
    
//    auto rp1 = new DenseMatrix<VT>*[numOutputs];
//    DenseMatrix<VT>*** res_cpp = &rp1;
    DenseMatrix<VT>*** res_cpp = new DenseMatrix<VT>**[numOutputs];
    auto offset = gpu_task_len;

    for (size_t i = 0; i < numOutputs; ++i) {
        res_cpp[i] = new DenseMatrix<VT>*;
        if(combines[i] == mlir::daphne::VectorCombine::ROWS) {
            (*res_cpp[i]) = static_cast<DenseMatrix<VT> *>((*res[i]))->slice(gpu_task_len, len);
        }
        else if(combines[i] == mlir::daphne::VectorCombine::COLS) {
            (*res_cpp[i]) = static_cast<DenseMatrix<VT> *>((*res[i]))->slice(0, outRows[i],gpu_task_len, len);
        }
        else {
            (*res_cpp[i]) = (*res[i]);
        }
    }

    uint64_t startChunk = gpu_task_len;
    uint64_t endChunk = gpu_task_len;
    auto chunkParam = 1;
    LoadPartitioning lp(STATIC, cpu_task_len, chunkParam, this->_numCPPThreads, false);
    while (lp.hasNextChunk()) {
        endChunk += lp.getNextChunk();
        q_cpp->enqueueTask(new CompiledPipelineTask<DenseMatrix<VT>>(CompiledPipelineTaskData<DenseMatrix<VT>>{
                funcs, inputs, numInputs, numOutputs, outRows, outCols, splits, combines, startChunk, endChunk,
                outRows, outCols, offset, ctx}, resLock, res_cpp));
        startChunk = endChunk;
    }
    q_cpp->closeInput();

    this->joinAll();

    this->combineOutputs(res, res_cuda, numOutputs, combines, gpu_task_len);

    for(size_t i = 0; i < numOutputs; ++i) {
//        (*res[i])->print(std::cout);
        if(combines[i] == mlir::daphne::VectorCombine::ROWS || combines[i] == mlir::daphne::VectorCombine::COLS)
            DataObjectFactory::destroy((*res_cpp[i]));
    }
}

template<typename VT>
void MTWrapper<DenseMatrix<VT>>::combineOutputs(DenseMatrix<VT>***& res_, DenseMatrix<VT>***& res_cuda_, size_t numOutputs,
        mlir::daphne::VectorCombine* combines, size_t gpu_rows) {
    for (size_t i = 0; i < numOutputs; ++i) {
        auto* res = static_cast<DenseMatrix<VT> *>((*res_[i]));
        auto* res_cuda = static_cast<DenseMatrix<VT> *>((*res_cuda_[i]));
        if (combines[i] == mlir::daphne::VectorCombine::ROWS) {
            const auto &const_res_cuda = *res_cuda;
            auto data_dest = res->getValues();
            CHECK_CUDART(cudaMemcpy(data_dest, const_res_cuda.getValuesCUDA(), const_res_cuda.bufferSize(),
                    cudaMemcpyDeviceToHost));
//            for(auto j=0; j < const_res_cuda.getNumItems(); j++)
//                std::cout << data_dest[j] << " ";
//            std::cout << std::endl;
            DataObjectFactory::destroy(res_cuda);
        }
        else if (combines[i] == mlir::daphne::VectorCombine::COLS) {
            const auto &const_res_cuda = *res_cuda;
            auto dst_base_ptr = res->getValues();
            auto src_base_ptr = const_res_cuda.getValuesCUDA();
            for(auto j = 0u; j < res_cuda->getNumRows(); ++j) {
                auto data_src = src_base_ptr + res_cuda->getRowSkip() * j;
//                auto data_src = src_base_ptr;
                auto data_dst = dst_base_ptr + res->getRowSkip() * j;
                CHECK_CUDART(cudaMemcpy(data_dst, data_src,res_cuda->getNumCols() * sizeof(VT),
                                        cudaMemcpyDeviceToHost));
            }
            DataObjectFactory::destroy(res_cuda);
        }
    }
}

template class MTWrapper<DenseMatrix<double>>;
template class MTWrapper<DenseMatrix<float>>;