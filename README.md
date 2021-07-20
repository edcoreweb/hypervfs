# hypervfs

A FUSE filesystem, offering a faster alternative to NFS for vm file sharing. (Observed 10x speed improvements in a Magento project)


## How it works

- uses hyperv or vmware sockets for communication, instead of TCP or UDP, thus bypassing the whole network stack
- has cache invalidation, meaning only the modified files are invalidated

## Todo

- [ ] ACL (make chmod, chown work, could use NTFS attributes to store the linux perms on windows)
- [ ] Cache management, cache purge, (we might want to limit the cache size, beacuse now everything is cached)
- [ ] Re-write the client as a kernel module, instead of using FUSE, thus gaining a perf boost
