#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int *buffer;
    int head;
    int tail;
    int size;
    int count;
} CircularBuffer;

void cb_init(CircularBuffer *cb, int size)
{
    cb->buffer = malloc(sizeof(int) * size);

    cb->size = size;
    cb->head = 0;
    cb->tail = 0;
    cb->count = 0;
}
int cb_is_empty(CircularBuffer *cb)
{
    return cb->count == 0;
}
int cb_is_full(CircularBuffer *cb)
{
    return cb->count == cb->size;
}
int cb_push(CircularBuffer *cb, int data)
{
    if (cb_is_full(cb))
        return -1;

    cb->buffer[cb->head] = data;

    cb->head = (cb->head + 1) % cb->size;

    cb->count++;

    return 0;
}
int cb_pop(CircularBuffer *cb, int *data)
{
    if (cb_is_empty(cb))
        return -1;

    *data = cb->buffer[cb->tail];

    cb->tail = (cb->tail + 1) % cb->size;

    cb->count--;

    return 0;
}
void cb_print(CircularBuffer *cb)
{
    int i;

    printf("Buffer: ");

    for (i = 0; i < cb->count; i++) {

        int index = (cb->tail + i) % cb->size;

        printf("%d ", cb->buffer[index]);
    }

    printf("\n");
}
int main()
{
    CircularBuffer cb;

    cb_init(&cb, 5);

    cb_push(&cb,10);
    cb_push(&cb,20);
    cb_push(&cb,30);

    cb_print(&cb);

    int x;

    cb_pop(&cb,&x);

    printf("Popped = %d\n",x);

    cb_push(&cb,40);
    cb_push(&cb,50);
    cb_push(&cb,60);

    cb_print(&cb);

    return 0;
}
