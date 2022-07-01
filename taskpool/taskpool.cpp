#include "taskpool.h"

TASKPOOL::TASKPOOL()
{
    m_q = new map<string, taskque*>;
}

TASKPOOL::~TASKPOOL()
{
    delete m_q;
}

TASKPOOL* TASKPOOL::GetInstance()
{
    static TASKPOOL tp;
    return &tp;
}

// �������������½�����
bool TASKPOOL::AddTask(string task, int t_size, int sock, string type)
{
    //�����ڸ������򴴽��ϴ����ļ�������
    if (m_q->find(task) == m_q->end()) {

        lck.lock();
        (*m_q)[task] = new taskque(t_size, type, task);
        lck.unlock();

    }
    return false;
}

//���һ�����ļ���δ�����Ƭ����Ϣ������-1��ʾʧ�ܣ�С��-1�����Ѿ�������
int TASKPOOL::GetSlice(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;

    return (*m_q)[task]->GetSlice();
}

void TASKPOOL::DelTask(string task)
{
    if ((*m_q)[task]->GetRecvSize() == (*m_q)[task]->GetAllSize()) {
        delete (*m_q)[task];
        m_q->erase(task);
    }
}

TASK_STATUS TASKPOOL::RecvTask(string task, int slice)
{
    if (m_q->find(task) == m_q->end())
        return REC_ERROR;
    TASK_STATUS rt = (*m_q)[task]->RecvTask(slice);
    return rt;
}

int TASKPOOL::GetSliceSize()
{
    return taskque::GetSliceSize();
}

int TASKPOOL::GetRecvSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->GetRecvSize();
}

int TASKPOOL::GetAllSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->GetAllSize();
}

string TASKPOOL::GetTaskType(string task)
{
    if (m_q->find(task) == m_q->end())
        return "";
    return (*m_q)[task]->m_type;
}

int TASKPOOL::GetTaskSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->m_size;
}

bool TASKPOOL::FindSlice(string task, int slice){
    if (m_q->find(task) == m_q->end())
        return false;
    return (*m_q)[task]->FindSlice(slice);
}