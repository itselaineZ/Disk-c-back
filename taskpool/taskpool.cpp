#include "taskpool.h"

TASKPOOL::TASKPOOL()
{
    m_assign = new map<string, taskque*>;
    m_rec = new map<string, taskque*>;
    m_sockpool = new map<string, cliset*>;
}

TASKPOOL::~TASKPOOL()
{
    delete m_assign;
    delete m_rec;
    delete m_sockpool;
}

TASKPOOL* TASKPOOL::GetInstance()
{
    static TASKPOOL tp;
    return &tp;
}

// 不存在任务则新建任务，并把client添加进上传集中
bool TASKPOOL::AddTask(string task, int t_size, int sock, string type)
{
    //不存在该任务，则创建上传该文件的任务
    if (m_assign->find(task) == m_assign->end()) {

        lck_asgn.lock();
        (*m_rec)[task] = new taskque(t_size, type, task, false);
        lck_asgn.unlock();
        
        lck_rcv.lock();
        (*m_assign)[task] = new taskque(t_size, type, task, true);
        lck_sock.unlock();

        lck_sock.lock();
        (*m_sockpool)[task] = new cliset;
        lck_rcv.unlock();
    }
    // 把用户添加进上传集
    (*m_sockpool)[task]->AddSock(sock);
    return false;
}

//获得一个该文件还未分配的片段信息，返回-1表示失败，小于-1代表已经分配完
int TASKPOOL::GetSlice(string task)
{
    if (m_assign->find(task) == m_assign->end())
        return -1;

    return (*m_assign)[task]->GetSlice();
}

void TASKPOOL::DelTask(string task)
{
    if ((*m_rec)[task]->GetRecvSize() == (*m_rec)[task]->GetAllSize()) {
        delete (*m_assign)[task];
        m_assign->erase(task);
        delete (*m_rec)[task];
        m_rec->erase(task);
        delete (*m_sockpool)[task];
        m_sockpool->erase(task);
    }
}

TASK_STATUS TASKPOOL::RecvTask(string task, int slice)
{
    if (m_rec->find(task) == m_rec->end())
        return REC_ERROR;
    TASK_STATUS rt = (*m_rec)[task]->RecvTask(slice);
    return rt;
}

int TASKPOOL::GetSliceSize()
{
    return taskque::GetSliceSize();
}

int TASKPOOL::GetRecvSize(string task)
{
    if (m_rec->find(task) == m_rec->end())
        return -1;
    return (*m_rec)[task]->GetRecvSize();
}

int TASKPOOL::GetAllSize(string task)
{
    if (m_rec->find(task) == m_rec->end())
        return -1;
    return (*m_rec)[task]->GetAllSize();
}

bool TASKPOOL::FindSock(int sock, string task)
{
    if (m_sockpool->find(task) == m_sockpool->end())
        return false;
    return (*m_sockpool)[task]->FindSock(sock);
}

string TASKPOOL::GetTaskType(string task)
{
    if (m_rec->find(task) == m_rec->end())
        return "";
    return (*m_rec)[task]->m_type;
}

int TASKPOOL::GetTaskSize(string task)
{
    if (m_rec->find(task) == m_rec->end())
        return -1;
    return (*m_rec)[task]->m_size;
}

void TASKPOOL::RemoveSock(string task, int sock)
{
    if (m_sockpool->find(task) == m_sockpool->end())
        return;
    if ((*m_sockpool)[task]->DelSock(sock))
        DelTask(task);
}

bool TASKPOOL::RecoverSlice(string task, int slice)
{
    if (m_assign->find(task) == m_assign->end())
        return false;
    (*m_assign)[task]->AddSlice(slice);
    return true;
}