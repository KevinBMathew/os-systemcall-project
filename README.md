# os-systemcall-project
This project includes 4 system calls that are added to the linux kernel and recompiled. (Currently working in kernel version 5.19.8 but will code itself will not be affected when porting to newer kernel versions.

The system calls enable **creation, deletion, publishing and subscribing to a topic** at a kernel level. This is to replicate ROS-like publisher-subscriber architecture for inter process communication.

## Note on LLM usage and other additional information
First Implementation is entirely human written (only non-human code is auto-completed code in VSCode). Even though the first implementation is functional, the second and final implementation is LLM-enhanced helping me get rid of race conditions I was unaware of as well as the `sys_delete_topic` being entirely LLM generated.
All documentation including README and pdf report generated from README are entirely human written.

OS version information : Fedora 36 VM running kernel version 5.19.8

## Table of Contents

- [os-systemcall-project](#os-systemcall-project)
  - [Note on LLM usage and other additional information](#note-on-llm-usage-and-other-additional-information)
  - [Table of Contents](#table-of-contents)
  - [Method](#method)
  - [File Index](#file-index)
  - [First Implementation](#first-implementation)
    - [Problems](#problems)
  - [Final Implementation](#final-implementation)
    - [How This Fixes Prior Problems](#how-this-fixes-prior-problems)
    - [Problems That Can Be Worked On](#problems-that-can-be-worked-on)
  - [Screenshots of Each Step](#screenshots-of-each-step)

## Method
This method adds four custom system calls directly into the Linux kernel source tree. Each syscall is defined using the `SYSCALL_DEFINE` macro and registered in the system call table as follows:
- `sys_create_topic` - creates a topic and registers it into the global linked list.
- `sys_publish` - copies a message from the user space to the kernel buffer and wakes all the waiting subsribers.
- `sys_subscribe` - blocks the calling process untill woken up by the publishing of a message then copies it to the user space.
- `sys_delete_topic` - removes a topic to free its resources (added in final implementation).

## File Index
 - `mysystemcall/pubsubold.c` - First implementation of system calls
 - `mysystemcall/pubsub.c` - Final implementation of system calls
 - `mysystemcall/Makefile`

## First Implementation
The first implementation covers the first 3 system calls. The topics are stored as a struct (pubsub_topic) in a global linked list. This is protected by a spinlock. Each topic has an integer ID, a message buffer, the buffer length, a wait queue for any sibscribers as well as a spinlock of its own for accessing the buffer.

`create_topic` validates ID, checks for dupilicates under the list lock, and then finally makes a new topic struct (allocates memory and initialises) and inserts it into the list.

`publish` copies the incoming message from the user into a new kernel buffer via copy_from_user and then swaps it into the topic under the topics spinlock, frees the old buffer, then calls `wake_up_all` to wake up all the sleeping subscribers.

`subscribe` looks up the topic ID, adds the calling process to wait queue and checks if a message already exists. IF not, `schedule()` lets the CPU perform other tasks until a publish wakes it up. At that time it will check fot pending signals and will copy mesage to user space using `copy_to_user`.
### Problems

## Final Implementation

### How This Fixes Prior Problems

### Problems That Can Be Worked On

## Screenshots of Each Step
