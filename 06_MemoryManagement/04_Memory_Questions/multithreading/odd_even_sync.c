#include <stdio.h>
#include <pthread.h>

#define MAX 100

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;

int current = 1;

void *print_odd(void *arg)
{
    while (current <= MAX) {
        pthread_mutex_lock(&mutex);

        while (current <= MAX && current % 2 == 0)
            pthread_cond_wait(&cond, &mutex);

        if (current <= MAX) {
            printf("ODD  : %d\n", current);
            current++;
            pthread_cond_signal(&cond);
        }

        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

void *print_even(void *arg)
{
    while (current <= MAX) {
        pthread_mutex_lock(&mutex);

        while (current <= MAX && current % 2 != 0)
            pthread_cond_wait(&cond, &mutex);

        if (current <= MAX) {
            printf("EVEN : %d\n", current);
            current++;
            pthread_cond_signal(&cond);
        }

        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

int main(void)
{
    pthread_t t_odd, t_even;

    pthread_create(&t_odd,  NULL, print_odd,  NULL);
    pthread_create(&t_even, NULL, print_even, NULL);

    pthread_join(t_odd,  NULL);
    pthread_join(t_even, NULL);

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);

    printf("Both threads done. Main exiting.\n");
    return 0;
}
