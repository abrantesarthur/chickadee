#ifndef CHICKADEE_U_SHM_HH
#define CHICKADEE_U_SHM_HH

int shmget(int key, size_t size = 0);
void* shmat(int shmid, const void* shmaddr);
int shmdt(const void* shmaddr);

#endif
