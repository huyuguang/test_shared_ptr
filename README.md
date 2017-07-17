几种构造shared_ptr方式的性能比较以及vs2017库代码的一个BUG

在10G网络下，每秒网卡的数据包大致在1M左右，如果用shared_ptr管理内存，那么需要尽可能快的分配和释放内存。
有3种方式得到shared_ptr：shared_ptr构造函数，make_shared，allocate_shared。
从原理上来说：

第一种方式需要先new一个对象，然后调用shared_ptr的构造函数，这个构造函数会再次调用一次new，分配一个小内存保存ref_count和deleter。
new一个对象可以通过实现分配好的内存池来优化。

第二种方式是直接由make_shared分配一块连续内存保存ref_count&deleter&object。因此这种方式无法利用预先分配的内存池。

第三种方式是通过allocator来分配一块连续内存，它的内存布局和make_shared类似，但是内存是allocator提供的，因此可以利用预先分配的内存池。

通常c库都为小内存分配做了优化，因此shared_ptr的那次小内存分配通常耗时是比较小的。但是内存分配通常带锁。
同样预分配的内存池的开销很小，也主要是锁操作。

以上几种方式的开销为：

shared_ptr(new T)：一次中等大小内存的分配/释放+一个小内存分配/释放

shared_ptr(pool.pop())：一次小内存分配/释放+带锁内存池分配/释放

make_shared()：一次中等大小内存的分配/释放

allocate_shared：带锁内存池分配/释放


由于shared_ptr本身的构造函数和T的构造函数是每种方式都必须有的，因此忽略这两个开销。
理论上来说，这几种方式的相互关系应该是allocate_shared > shared_ptr_with_pool > raw_new_delete > make_shared > shared_ptr_with_new。

allocate_shared比shared_ptr_with_pool快是因为allocate_shared同样用pool，但是少了一次小内存分配。

raw_new_delete比make_shared快是因为少了shared_ptr的构造函数调用

make_shared比shared_ptr_with_new是因为make_shared只分配一次内存




在gcc4.8.5 centos6.2上测试，性能情况的确如预期：
g++ -std=c++11 a.cpp -O3 -g

avg_test_make_shared: 1148

avg_test_shared_ptr_with_new: 1164

avg_test_shared_ptr_with_pool: 407

avg_test_allocate_shared: 279

avg_test_new_delete: 1090

avg_test_pool: 7

各种方法的相互关系完全符合预期。


在vs2017+win10, x64 release上测试，情况颇有不同

avg_test_make_shared: 723

avg_test_shared_ptr_with_new: 799

avg_test_shared_ptr_with_pool: 212

avg_test_allocate_shared: 403

avg_test_new_delete: 660

avg_test_pool: 65

这里令人十分不解的是，为什么allocate_shared会这么慢？

经过仔细检查，发现这是因为vs2017的allocate_shared实现有BUG。
它在内部试图在allocator传给它的内存上place new一个std::_Align_type，由于此处用的是perfect-forwarding构造_Align_type，因此编译器认为此时需要做value-initialize，但是由于_Align_type没有构造函数， 最后编译器生成memset(addr, 0, sizeof(T))来做value-initialize。

也就是说，vs2017的allocate_shared在每次调用T的构造函数之前，先做了一次memset(0)。就是这个memset(0)严重的降低了allocate_shared的性能。要修复这个BUG，要到vc的include目录找到type_traits文件，找到_Align_type的定义：


template<class _Ty, size_t _Len> union _Align_type
       
       {      // union with size _Len bytes and alignment of _Ty
       
       _Ty _Val;
       
       char _Pad[_Len];
       
       };


给它增加一个空构造函数就可以了：

template<class _Ty, size_t _Len> union _Align_type
       
       {      // union with size _Len bytes and alignment of _Ty
       
       _Ty _Val;
       
       char _Pad[_Len];
       
       _Align_type(){}
       
       };

修改了这个头文件之后再测试，结果如下：

avg_test_make_shared: 736

avg_test_shared_ptr_with_new: 802

avg_test_shared_ptr_with_pool: 215

avg_test_allocate_shared: 206

avg_test_new_delete: 679

avg_test_pool: 65


可以看到allocate_shared符合预期的比shared_ptr_with_pool略快一点。这似乎是因为vc对小内存分配的优化做的特别好，因此shared_ptr_with_pool额外的那次小内存分配开销很小。
