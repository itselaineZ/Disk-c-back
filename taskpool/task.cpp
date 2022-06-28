#include "task.h"

taskque::taskque() { }
taskque::taskque(int t_size, string type, string name, bool init)
{
    m_size = t_size;
    m_type = type;
    m_name = name;
    m_count = t_size / SLICE_SIZE + (t_size / SLICE_SIZE * SLICE_SIZE < t_size);
    if (init) {
        for (int i = 0; i < m_count; ++i)
            m_q.push(i);
    }
}
taskque::~taskque() { }

// 如果小于0则表示已经分配完了
int taskque::GetSlice()
{
    int rt = -1;
    lck.lock();
    if (!m_q.empty()) {
        rt = m_q.front();
        m_q.pop();
    }
    lck.unlock();
    return rt * SLICE_SIZE;
}

TASK_STATUS taskque::RecvTask(int slice)
{
    lck.lock();
    m_q.push(slice);
    lck.unlock();
    if (m_q.size() == m_count) {
        FILEUTIL fu;
        bool rt = fu.MergeFile(m_name.c_str(), m_count);
        if (!rt)
            return MERGE_FAILED;
        return MERGE_SUCCESS;
    }
    return RECVING;
}

int taskque::GetRecvSize()
{
    return m_q.size();
}

int taskque::GetAllSize()
{
    return m_count;
}

int taskque::GetSliceSize()
{
    return SLICE_SIZE;
}

void taskque::AddSlice(int slice)
{
    lck.lock();
    m_q.push(slice);
    lck.unlock();
}

cliset::cliset() { }
cliset::~cliset() { }

void cliset::AddSock(int sock)
{
    lck.lock();
    m_st.insert(sock);
    lck.unlock();
}

bool cliset::FindSock(int sock)
{
    return m_st.find(sock) != m_st.end();
}

// 删除后set为空返回1，不为空则返回0
bool cliset::DelSock(int sock)
{
    lck.lock();
    m_st.erase(sock);
    lck.unlock();
    return m_st.size() == 0;
}