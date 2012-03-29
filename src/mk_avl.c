#include "mk_avl.h"
#include "mk_utils.h"

_mk_avl_node *mk_avl_insert(_mk_avl_node *root, _mk_avl_node *ins)
{
    if(!ins)
        return NULL;
    if (!root)
        return ins;

    if (root->value > ins->value) {
        root->left = mk_avl_insert(root->left, ins);
        root->l_height = MAX(root->left->l_height, root->left->r_height) + 1;
    }
    else {
        root->right = mk_avl_insert(root->right, ins);
        root->r_height = MAX(root->right->l_height, root->right->r_height) + 1;
    }

    return mk_avl_balance_node(root);
}


_mk_avl_node *mk_avl_delete_node(_mk_avl_node *root, _mk_avl_node *del)
{
    return mk_avl_delete_val(root, del->value);

}

_mk_avl_node *mk_avl_delete_val(_mk_avl_node *root, int val)
{
    _mk_avl_node *t1, *ret;

    ret = root;

    if (root->value > val) {
     root->left = mk_avl_delete_val(root->left, val);
     if (root->left) {
         root->l_height = MAX(root->left->l_height, root->left->r_height) + 1;
     }
     else {
         root->l_height = 0;
     }
    }

    if (root->value < val) {
        root->right = mk_avl_delete_val(root->right, val);
        if (root->right) {
            root->r_height = MAX(root->right->l_height, root->right->r_height) + 1;
        }
        else {
            root->r_height = 0;
        }
    }

    if (root->value == val) {
        if ((root->l_height * root->r_height) <= 1) {
            if (root->left && root->right) {
                t1 = root->left;
                t1->right = root->right;
                t1->r_height=1;
                ret = t1;
            }

                ret = root->left ? root->left : root->right;
        }
        else{
            //remove the rightmost node in the leftmost tree. on the way back start balancing the tree
            ret = mk_avl_get_rmost(root->left);
            root->left = mk_avl_delete_rmost(root->left);

            ret->right = root->right;
            ret->left = root->left;
            ret->r_height = root->r_height;
            ret->l_height = root->l_height;
        }
        root->left = NULL;
        root->right = NULL;
        root->l_height = 0;
        root->r_height = 0;
    }

    if(!ret)
        return ret;
    else
        return mk_avl_balance_node(ret);

}

_mk_avl_node *mk_avl_get_rmost(_mk_avl_node *node){
    _mk_avl_node *t1;

    t1=node;
    if(!t1)
        return t1;

    while(t1->right){
        t1=t1->right;
    }
    return t1;
}

_mk_avl_node *mk_avl_delete_rmost(_mk_avl_node *node){
    _mk_avl_node *t1;

    if (!node->right) {
        return NULL;
    }

    t1 = mk_avl_delete_rmost(node->right);
    if (!t1) {
        node->right = NULL;
        node->r_height = 0;
    }
    else{
        node->right = t1;
        node->r_height = MAX(node->right->l_height, node->right->r_height) + 1;
    }
    return mk_avl_balance_node(node);

}

_mk_avl_node *mk_avl_balance_node(_mk_avl_node *node)
{


    int rotate = node->r_height - node->l_height;
    _mk_avl_node *t1;


    if (rotate > 1) {
        if (node->right->l_height > node->right->r_height) {

            t1 = node->right;

            node->right = t1->left;
            t1->left = node->right->right;
            node->right->right = t1;



            if(t1->left)
                t1->l_height = MAX(t1->left->l_height, t1->left->r_height) + 1;
            else
                t1->l_height=0;

            node->right->r_height = MAX(t1->l_height, t1->r_height) + 1;
        }
        t1 = node->right;
        node->right = t1->left;
        t1->left = node;

        if (node->right)
            node->r_height = MAX(node->right->l_height, node->right->r_height) + 1;
        else
            node->r_height = 0;

        t1->l_height = MAX(t1->left->l_height, t1->left->r_height) + 1;
        t1->r_height = MAX(t1->right->l_height, t1->right->r_height) + 1;

        return t1;
    }

    if(rotate < -1){
        if (node->left->r_height > node->left->l_height) {

            t1 = node->left;

            node->left = t1->right;
            t1->right = node->left->left;
            node->left->left = t1;


            if(t1->right)
                t1->r_height = MAX(t1->right->l_height, t1->right->r_height) + 1;
            else
                t1->r_height = 0;
            node->left->l_height = MAX(t1->l_height, t1->r_height) + 1;
        }
        t1 = node->left;
        node->left = t1->right;
        t1->right = node;


        if(node->left)
            node->l_height = MAX(node->left->l_height, node->left->r_height) + 1;
        else
            node->l_height = 0;

        t1->l_height = MAX(t1->left->l_height, t1->left->r_height) + 1;
        t1->r_height = MAX(t1->right->l_height, t1->right->r_height) + 1;

        return t1;
    }

    return node;
}
