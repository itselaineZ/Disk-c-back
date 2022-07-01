#include "task.h"

taskque::taskque() { }
taskque::taskque(int t_size, string type, string name)
{
    m_size = t_size;
    m_type = type;
    m_name = name;
    m_count = t_size / SLICE_SIZE + (t_size / SLICE_SIZE * SLICE_SIZE < t_size);
    for (int i = 0; i < m_count; ++i)
        m_assign.push(i);
}
taskque::~taskque() { }

// 如果小于0则表示已经分配完了
int taskque::GetSlice()
{
    int rt = -1;
    lck.lock();
    while (!m_assign.empty()) {
        rt = m_assign.front();
        m_assign.pop();
        // 查找一片还没有接收到的片段分配出去
        if (m_rec.find(rt) == m_rec.end())
            break;
    }
    m_assign.push(rt);
    lck.unlock();
    return rt * SLICE_SIZE;
}

TASK_STATUS taskque::RecvTask(int slice)
{
    lck.lock();
    m_rec.insert(slice);
    lck.unlock();
    if (m_rec.size() == m_count) {
        printf("to merge\n");
        // FILEUTIL fu;
        // bool rt = fu.MergeFile(m_name.c_str(), m_count);
        // if (!rt)
        //     return MERGE_FAILED;
        return MERGE_SUCCESS;
    }
    return RECVING;
}

int taskque::GetRecvSize()
{
    return m_rec.size();
}

int taskque::GetAllSize()
{
    return m_count;
}

int taskque::GetSliceSize()
{
    return SLICE_SIZE;
}

bool taskque::FindSlice(int slice)
{
    return m_rec.find(slice) != m_rec.end();
}