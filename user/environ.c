#include "user.h"

struct key_value_pair {
        char *key;
        char *value;
};

struct hash_table {
        struct key_value_pair **kvp;
        int sz;
};

static struct hash_table ht;

/**
 * @description: Create a hash code base on key
 * @param {char} *key: key string
 * @return {*} the hash code
 */
static unsigned int hash(const char *key)
{
        unsigned int hash = 0;
        while (*key) {
                hash = (hash << 5) + *key++;
        }
        return hash % ht.sz;
}

void init_env(void)
{
        ht.sz = 9;
        ht.kvp = malloc(sizeof(struct key_value_pair*) * 9);
}

char* getenv(const char *key)
{
        unsigned int index;
        struct key_value_pair *kvp;
        /* Get the hash code */
        index = hash(key);

        kvp = ht.kvp[index];

        if(!kvp)
                return 0;
                
        return kvp->value;
}

void setenv(const char *key, const char *value)
{
        unsigned int index, flag;
        struct key_value_pair *kvp;
        char *t;

        /* Get the hash code */
        index = hash(key);

        /* overflow? */
        if(index >= ht.sz)
                return;
        /* Get the pointer the index points */
        kvp = ht.kvp[index];

        if(!kvp) {
                /* No the key flag */
                flag = 1;

                kvp = malloc(sizeof(struct key_value_pair));
                if(!kvp)
                        return;
                memset(kvp, 0, sizeof(struct key_value_pair));

                /* alloc the key string memory failed so we free the structure */
                kvp->key = malloc(strlen(key) + 1);
                /* Free the kvp memory */
                if(!kvp->key)
                        free(kvp);
                strcpy(kvp->key, key);
                /* Set the kvp */
                ht.kvp[index] = kvp;
        } else {
                flag = 0;
                free(kvp->value);
        }

        /* 
                Use `t` instead of `kvp->value` because if the memory alloc failed
                this body doesn't change where the kvp->value points
        */
        t = malloc(strlen(value) + 1);

        if(!t) {
                /* The key-value-pair doesn't have original value so free key */
                if(flag) {
                        free(kvp->key);
                        free(kvp);
                        ht.kvp[index] = 0;
                } else {
                        return;
                }
        }
        strcpy(t, value);
        kvp->value = t;
}