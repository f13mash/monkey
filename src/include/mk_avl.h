
#ifndef MK_AVL_H_
#define MK_AVL_H_


#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifndef NULL
#define NULL 0
#endif


/*
 * this is the modified version of the linux kernel macro container_of.
 * previous version didn't check for ptr  to be null hence returning a non-null address for the type passed leading to segf
 */
#ifndef container_of_null_handle
#define container_of_null_handle(ptr, type, member) ({                      \
      const typeof( ((type *)0)->member ) *__mptr = (ptr);      \
      (ptr)? (type *)( (char *)__mptr - offsetof(type,member) ) : NULL;})
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif


typedef struct mk_avl_node
{
    struct mk_avl_node *left, *right;
    int value;
    int l_height, r_height;
} _mk_avl_node;

#define mk_avl_get_val_entry( root, value, type, member ) container_of_null_handle( mk_avl_get_val(root, value), type, member )


static inline _mk_avl_node *mk_avl_get_val(_mk_avl_node *root, int value)
{

    while(root && root->value != value){
        if(root->value > value)
            root = root->left;
        else
            root = root->right;
    }

    return root;
}

_mk_avl_node *mk_avl_balance_node(_mk_avl_node *node);
_mk_avl_node *mk_avl_insert(_mk_avl_node *root, _mk_avl_node *ins);
_mk_avl_node *mk_avl_delete_node(_mk_avl_node *root, _mk_avl_node *del);
_mk_avl_node *mk_avl_delete_val(_mk_avl_node *root, int val);
_mk_avl_node *mk_avl_get_rmost(_mk_avl_node *node);
_mk_avl_node *mk_avl_delete_rmost(_mk_avl_node *node);

#endif /* MK_AVL_H_ */
