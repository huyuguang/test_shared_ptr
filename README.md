# test_shared_ptr
shared_ptr performance test

loop 1000000 times to create and destroy shared_ptr.

the time is ms.

make_shared: 473
make_shared: 443
make_shared: 452
======
shared_ptr with pool: 145
shared_ptr with pool: 140
shared_ptr with pool: 140
======
shared_ptr with new: 513
shared_ptr with new: 560
shared_ptr with new: 538
======
allocate_shared: 304
allocate_shared: 289
allocate_shared: 312
======
raw new&delete: 457
raw new&delete: 439
raw new&delete: 431

what make confused is why allocate_shared slower than shared_ptr with pool?
