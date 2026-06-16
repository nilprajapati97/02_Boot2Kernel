#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SAPIO");
MODULE_DESCRIPTION("Custom Slab Cache Example");

/* Custom data structure */
struct my_object {
    int id;
    char name[32];
    unsigned long timestamp;
    void *data;
};

static struct kmem_cache *my_cache;

/* Constructor - called when object is allocated */
static void my_constructor(void *obj)
{
    struct my_object *my_obj = obj;
    my_obj->id = 0;
    my_obj->timestamp = 0;
    my_obj->data = NULL;
    memset(my_obj->name, 0, sizeof(my_obj->name));
    pr_info("Constructor called for object at %p\n", obj);
}

static int __init custom_slab_init(void)
{
    struct my_object *obj1, *obj2, *obj3;

    pr_info("Creating custom slab cache\n");

    /* Create custom cache */
    my_cache = kmem_cache_create("my_cache",
                                 sizeof(struct my_object),
                                 0,
                                 SLAB_HWCACHE_ALIGN | SLAB_POISON,
                                 my_constructor);
    if (!my_cache) {
        pr_err("Failed to create cache\n");
        return -ENOMEM;
    }

    pr_info("Cache created successfully\n");

    /* Allocate objects from cache */
    obj1 = kmem_cache_alloc(my_cache, GFP_KERNEL);
    if (obj1) {
        obj1->id = 1;
        snprintf(obj1->name, sizeof(obj1->name), "Object-1");
        pr_info("Allocated obj1: id=%d, name=%s\n", obj1->id, obj1->name);
    }

    obj2 = kmem_cache_alloc(my_cache, GFP_KERNEL);
    if (obj2) {
        obj2->id = 2;
        snprintf(obj2->name, sizeof(obj2->name), "Object-2");
        pr_info("Allocated obj2: id=%d, name=%s\n", obj2->id, obj2->name);
    }

    obj3 = kmem_cache_alloc(my_cache, GFP_KERNEL);
    if (obj3) {
        obj3->id = 3;
        snprintf(obj3->name, sizeof(obj3->name), "Object-3");
        pr_info("Allocated obj3: id=%d, name=%s\n", obj3->id, obj3->name);
    }

    /* Free objects back to cache */
    if (obj1)
        kmem_cache_free(my_cache, obj1);
    if (obj2)
        kmem_cache_free(my_cache, obj2);
    if (obj3)
        kmem_cache_free(my_cache, obj3);

    pr_info("Objects freed back to cache\n");

    return 0;
}

static void __exit custom_slab_exit(void)
{
    if (my_cache) {
        kmem_cache_destroy(my_cache);
        pr_info("Cache destroyed\n");
    }
}

module_init(custom_slab_init);
module_exit(custom_slab_exit);
