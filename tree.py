from __future__ import annotations

from dataclasses import dataclass
from pprint import pprint
import random
from typing import Any, Hashable

type Tree = Node | None


@dataclass
class Node:
    key: Any
    value: Any
    left: Tree
    right: Tree


def _iou_helper(tree: Tree, key: Hashable, value: Any, hval: int) -> Node:
    if tree is None:
        return Node(key, value, None, None)
    if tree.key == key:
        return Node(key, value, tree.left, tree.right)
    is_left = hval < hash(tree.key)
    newnode = Node(tree.key, tree.value, tree.left, tree.right)
    if is_left:
        newnode.left = _iou_helper(newnode.left, key, value, hval)
    else:
        newnode.right = _iou_helper(newnode.right, key, value, hval)
    return newnode


def with_inserted(tree: Tree, key: Hashable, value: Any) -> Node:
    return _iou_helper(tree, key, value, hash(key))


def dict2tree(d: dict) -> Tree:
    tree = None
    for k, v in d.items():
        tree = with_inserted(tree, k, v)
    return tree


def treeget(t: Tree, k: Hashable) -> Any:
    hval = hash(k)
    while t:
        if t.key == k:
            return t.value
        t = t.left if hval < hash(t.key) else t.right
    raise KeyError(k)


testdict = {random.randrange(100): random.randrange(100) for _ in range(20)}
test_tree = dict2tree(testdict)

pprint(test_tree)
assert all(treeget(test_tree, k) == v for k, v in testdict.items())
print("get is ok")
