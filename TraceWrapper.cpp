#include "TraceWrapper.hpp"
#include <stdio.h>

using namespace contech;

TraceWrapper::TraceWrapper(char* fname)
{
    assert(fname != NULL);

    ct_file* taskGraphIn  = create_ct_file_r(fname);
    if (taskGraphIn == NULL) {
        fprintf(stderr, "ERROR: Couldn't open input file\n");
        exit(1);
    }
    
    tg = TaskGraph::initFromFile(taskGraphIn);
    
    lastOpTime = 0;
}

TraceWrapper::~TraceWrapper()
{
    delete tg;
}

// getNextMemoryRequest
//   Populates a MemReq struct with the next memory request
//   returns 0 if no request is returned
int TraceWrapper::getNextMemoryRequest(MemReq* memR)
{
    assert(memR != NULL);
    
    if (memReqQ.empty())
    {
        if (!populateQueue())
        {
            return 0;
        }
    }
    
    MemReqContainer nextReq = memReqQ.top();
    memReqQ.pop();
    *memR = nextReq.req;

    lastOpTime = nextReq.reqTime;
    
    return 1;
}

//
// Scan the vector of successor tasks and find the next in sequence
//
TaskId TraceWrapper::getSequenceTask(vector<TaskId>& succ, ContextId selfId)
{
    TaskId possible_succ = 0;

    for (auto i : succ)
    {
        if (i.getContextId() == selfId)
        {
            if (possible_succ == 0 ||
                i < possible_succ)
            {
                possible_succ = i;
            }
        }
    }
    
    return possible_succ;
}

// populateQueue
//   Populates the memory request queue with additional requests
int TraceWrapper::populateQueue()
{
    int addedMemOps = 0;

    // foreach task in graph
    //   Update running tasks based on the advancement in time represented by
    //   the new task
    while (Task* currentTask = tg->getNextTask())
    {
        TaskId ctui = currentTask->getTaskId();
        ContextId ctci = currentTask->getContextId();
        
        ct_timestamp start = currentTask->getStartTime();
        ct_timestamp req = currentTask->getEndTime();

        // Iterate through every basic block, older than start
        for (auto hs_b = contechState.begin(), hs_e = contechState.end(); hs_b != hs_e; ++hs_b)
        {
            ctid_current_state* tempState = (hs_b->second);
            if (tempState->terminated == true) continue;
            Task* t = tempState->currentTask;
            ct_timestamp tempCurrent = tempState->taskCurrTime;
            ct_timestamp tempRate = tempState->taskRate;
            auto f = tempState->currentBB;
            string s = t->getTaskId().toString();
            
            //
            // tempRate = 0 -> no basic blocks or currentTask->time == nextTask->time
            //   Still, process the basic blocks
            //
            for (auto e = tempState->currentBBCol.end(); 
                 (tempCurrent <= start) && (f != e); ++f)
            {
                BasicBlockAction tbb = *f;
                
                // Push MemOps onto queue
                auto memOps = f.getMemOps();
                MemReqContainer tReq;
                for (auto iReq = memOps.begin(), eReq = memOps.end(); iReq != eReq; ++iReq)
                {
                    MemoryAction ma = *iReq;
                
                    tReq.req.ctid = (unsigned int) hs_b->first;
                    tReq.req.isWrite = (ma.type == action_type_mem_write);
                    tReq.req.numOfBytes = (0x1 << ma.pow_size);
                    tReq.req.address = ma.addr;
                    tReq.req.bbid = tbb.basic_block_id;
                    tReq.reqTime = tempCurrent;
                
                    memReqQ.push(tReq);
                    addedMemOps++;
                }
                
                tempCurrent += tempRate;
            }
            
            // Should the task switch always be from currentTask == next?
            //   o.w. The last basic block "spans" start
            if (ctui == tempState->nextTaskId)
            {
                bool tBlock = tempState->blocked;
                
                //
                // Termination condition if the contech IDs change
                //   Or 0 is the successor task
                //
                if (t->getContextId() != tempState->nextTaskId.getContextId()
                    || tempState->nextTaskId == 0)
                {
                    delete t;
                    tempState->terminated = true;
                    continue;
                }
                
                // Is the new task running or doing something synchronizing?
                if (ctui == tempState->nextTaskId)
                {
                    tempState->currentTask = currentTask;
                }
                else
                {
                    tempState->currentTask = tg->getTaskById((tempState->nextTaskId));
                }
                
                assert(tempState->currentTask != NULL);
                
                if (tempState->currentTask->getType() == task_type_basic_blocks)
                    tempState->blocked = false;
                else
                    tempState->blocked = true;
                
                // If there is no continuation, then this task has terminated
                tempState->nextTaskId = getSequenceTask(tempState->currentTask->getSuccessorTasks(),
                                                        tempState->currentTask->getContextId());
                
                tempState->taskCurrTime = tempState->currentTask->getStartTime();
                tempState->currentBBCol = tempState->currentTask->getBasicBlockActions();
                tempState->currentBB = tempState->currentBBCol.begin();
                
                    
                // If the task is blocked, then it is not running.
                //  o.w. compute the task rate for this next task
                if (tempState->blocked == true)
                {
                    tempState->taskRate = 0;
                }
                else
                {
                    int bbc = tempState->currentTask->getBBCount();
                    if (bbc == 0)
                        bbc = 1;
                    tempState->taskRate = (tempState->currentTask->getEndTime() - tempState->taskCurrTime/* + bbc - 1*/) 
                                                / (bbc); 
                }
                delete t;
            }
            else if (tempCurrent < start)
            {
                tempState->blocked = true;
            }
            else
            {
                tempState->currentBB = f;
                tempState->taskCurrTime = tempCurrent;
            }
        }  // end of foreach task in contechState
        
        //
        // If ctci is not in contechState, then it is a new contech
        //   TODO: Due to barriers, it is possible that we'll need to "unterminate" some states
        //
        if (contechState.find(ctci) == contechState.end())
        {
            ctid_current_state* tempState = new ctid_current_state;
            
            tempState->terminated = false;
            tempState->taskCurrTime = start;
            tempState->currentBBCol = currentTask->getBasicBlockActions();
            tempState->currentBB = tempState->currentBBCol.begin();
            tempState->currentTask = currentTask;
            tempState->nextTaskId = getSequenceTask(tempState->currentTask->getSuccessorTasks(),
                                                    tempState->currentTask->getContextId());
            
            if (currentTask->getType() == task_type_basic_blocks)
            {
                tempState->blocked = false;
                tempState->taskRate = (tempState->currentTask->getEndTime() - start) / (currentTask->getBBCount());
            }
            else
            {
                tempState->blocked = true;
                tempState->taskRate = 0;
            }
            contechState[ctci] = tempState;
        }
        
        if (addedMemOps > 0) break;
    }
    
    return addedMemOps;
}