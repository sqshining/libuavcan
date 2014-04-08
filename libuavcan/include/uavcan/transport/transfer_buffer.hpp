/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#pragma once

#include <algorithm>
#include <limits>
#include <uavcan/stdint.hpp>
#include <uavcan/error.hpp>
#include <uavcan/transport/frame.hpp>
#include <uavcan/linked_list.hpp>
#include <uavcan/dynamic_memory.hpp>
#include <uavcan/impl_constants.hpp>
#include <uavcan/debug.hpp>

namespace uavcan
{
UAVCAN_PACKED_BEGIN
/**
 * API for transfer buffer users.
 */
class UAVCAN_EXPORT ITransferBuffer
{
public:
    virtual ~ITransferBuffer() { }

    virtual int read(unsigned offset, uint8_t* data, unsigned len) const = 0;
    virtual int write(unsigned offset, const uint8_t* data, unsigned len) = 0;
};

/**
 * Internal for TransferBufferManager
 */
class UAVCAN_EXPORT TransferBufferManagerKey
{
    NodeID node_id_;
    uint8_t transfer_type_;

public:
    TransferBufferManagerKey()
        : transfer_type_(TransferType(0))
    {
        assert(isEmpty());
    }

    TransferBufferManagerKey(NodeID node_id, TransferType ttype)
        : node_id_(node_id)
        , transfer_type_(ttype)
    {
        assert(!isEmpty());
    }

    bool operator==(const TransferBufferManagerKey& rhs) const
    {
        return node_id_ == rhs.node_id_ && transfer_type_ == rhs.transfer_type_;
    }

    bool isEmpty() const { return !node_id_.isValid(); }

    NodeID getNodeID() const { return node_id_; }
    TransferType getTransferType() const { return TransferType(transfer_type_); }

    std::string toString() const;
};

/**
 * Internal for TransferBufferManager
 */
class UAVCAN_EXPORT TransferBufferManagerEntry : public ITransferBuffer, Noncopyable
{
    TransferBufferManagerKey key_;

protected:
    virtual void resetImpl() = 0;

public:
    TransferBufferManagerEntry() { }

    TransferBufferManagerEntry(const TransferBufferManagerKey& key)
        : key_(key)
    { }

    const TransferBufferManagerKey& getKey() const { return key_; }
    bool isEmpty() const { return key_.isEmpty(); }

    void reset(const TransferBufferManagerKey& key = TransferBufferManagerKey())
    {
        key_ = key;
        resetImpl();
    }
};

/**
 * Resizable gather/scatter storage.
 * reset() call releases all memory blocks.
 * Supports unordered write operations - from higher to lower offsets
 */
class UAVCAN_EXPORT DynamicTransferBufferManagerEntry
    : public TransferBufferManagerEntry
    , public LinkedListNode<DynamicTransferBufferManagerEntry>
{
    struct Block : LinkedListNode<Block>
    {
        enum { Size = MemPoolBlockSize - sizeof(LinkedListNode<Block>) };
        uint8_t data[Size];

        static Block* instantiate(IAllocator& allocator);
        static void destroy(Block*& obj, IAllocator& allocator);

        void read(uint8_t*& outptr, unsigned target_offset,
                  unsigned& total_offset, unsigned& left_to_read);
        void write(const uint8_t*& inptr, unsigned target_offset,
                   unsigned& total_offset, unsigned& left_to_write);
    };

    IAllocator& allocator_;
    LinkedListRoot<Block> blocks_;    // Blocks are ordered from lower to higher buffer offset
    unsigned max_write_pos_;
    const unsigned max_size_;

    void resetImpl();

public:
    DynamicTransferBufferManagerEntry(IAllocator& allocator, unsigned max_size)
        : allocator_(allocator)
        , max_write_pos_(0)
        , max_size_(max_size)
    {
        StaticAssert<(Block::Size > 8)>::check();
        IsDynamicallyAllocatable<Block>::check();
        IsDynamicallyAllocatable<DynamicTransferBufferManagerEntry>::check();
    }

    ~DynamicTransferBufferManagerEntry()
    {
        resetImpl();
    }

    static DynamicTransferBufferManagerEntry* instantiate(IAllocator& allocator, unsigned max_size);
    static void destroy(DynamicTransferBufferManagerEntry*& obj, IAllocator& allocator);

    int read(unsigned offset, uint8_t* data, unsigned len) const;
    int write(unsigned offset, const uint8_t* data, unsigned len);
};
UAVCAN_PACKED_END

/**
 * Standalone static buffer
 */
template <unsigned Size>
class UAVCAN_EXPORT StaticTransferBuffer : public ITransferBuffer
{
    uint8_t data_[Size];
    unsigned max_write_pos_;

public:
    StaticTransferBuffer()
        : max_write_pos_(0)
    {
        StaticAssert<(Size > 0)>::check();
        std::fill(data_, data_ + Size, 0);
    }

    int read(unsigned offset, uint8_t* data, unsigned len) const
    {
        if (!data)
        {
            assert(0);
            return -ErrInvalidParam;
        }
        if (offset >= max_write_pos_)
        {
            return 0;
        }
        if ((offset + len) > max_write_pos_)
        {
            len = max_write_pos_ - offset;
        }
        assert((offset + len) <= max_write_pos_);
        std::copy(data_ + offset, data_ + offset + len, data);
        return len;
    }

    int write(unsigned offset, const uint8_t* data, unsigned len)
    {
        if (!data)
        {
            assert(0);
            return -ErrInvalidParam;
        }
        if (offset >= Size)
        {
            return 0;
        }
        if ((offset + len) > Size)
        {
            len = Size - offset;
        }
        assert((offset + len) <= Size);
        std::copy(data, data + len, data_ + offset);
        max_write_pos_ = std::max(offset + len, max_write_pos_);
        return len;
    }

    void reset()
    {
        max_write_pos_ = 0;
#if UAVCAN_DEBUG
        std::fill(data_, data_ + Size, 0);
#endif
    }

    uint8_t* getRawPtr() { return data_; }
    const uint8_t* getRawPtr() const { return data_; }

    unsigned getMaxWritePos() const { return max_write_pos_; }
    void setMaxWritePos(unsigned value) { max_write_pos_ = value; }
};

/**
 * Statically allocated storage for buffer manager
 */
template <unsigned Size>
class UAVCAN_EXPORT StaticTransferBufferManagerEntry : public TransferBufferManagerEntry
{
    StaticTransferBuffer<Size> buf_;

    void resetImpl()
    {
        buf_.reset();
    }

public:
    int read(unsigned offset, uint8_t* data, unsigned len) const
    {
        return buf_.read(offset, data, len);
    }

    int write(unsigned offset, const uint8_t* data, unsigned len)
    {
        return buf_.write(offset, data, len);
    }

    bool migrateFrom(const TransferBufferManagerEntry* tbme)
    {
        if (tbme == NULL || tbme->isEmpty())
        {
            assert(0);
            return false;
        }

        // Resetting self and moving all data from the source
        TransferBufferManagerEntry::reset(tbme->getKey());
        const int res = tbme->read(0, buf_.getRawPtr(), Size);
        if (res < 0)
        {
            TransferBufferManagerEntry::reset();
            return false;
        }
        buf_.setMaxWritePos(res);
        if (res < int(Size))
        {
            return true;
        }

        // Now we need to make sure that all data can fit the storage
        uint8_t dummy = 0;
        if (tbme->read(Size, &dummy, 1) > 0)
        {
            TransferBufferManagerEntry::reset();            // Damn, the buffer was too large
            return false;
        }
        return true;
    }
};

/**
 * Manages different storage types (static/dynamic) for transfer reception logic.
 */
class UAVCAN_EXPORT ITransferBufferManager
{
public:
    virtual ~ITransferBufferManager() { }
    virtual ITransferBuffer* access(const TransferBufferManagerKey& key) = 0;
    virtual ITransferBuffer* create(const TransferBufferManagerKey& key) = 0;
    virtual void remove(const TransferBufferManagerKey& key) = 0;
};

/**
 * Convinience class.
 */
class UAVCAN_EXPORT TransferBufferAccessor
{
    ITransferBufferManager& bufmgr_;
    const TransferBufferManagerKey key_;

public:
    TransferBufferAccessor(ITransferBufferManager& bufmgr, TransferBufferManagerKey key)
        : bufmgr_(bufmgr)
        , key_(key)
    {
        assert(!key.isEmpty());
    }
    ITransferBuffer* access() { return bufmgr_.access(key_); }
    ITransferBuffer* create() { return bufmgr_.create(key_); }
    void remove() { bufmgr_.remove(key_); }
};

/**
 * Buffer manager implementation.
 */
template <unsigned MaxBufSize, unsigned NumStaticBufs>
class UAVCAN_EXPORT TransferBufferManager : public ITransferBufferManager, Noncopyable
{
    typedef StaticTransferBufferManagerEntry<MaxBufSize> StaticBufferType;

    StaticBufferType static_buffers_[NumStaticBufs];
    LinkedListRoot<DynamicTransferBufferManagerEntry> dynamic_buffers_;
    IAllocator& allocator_;

    StaticBufferType* findFirstStatic(const TransferBufferManagerKey& key)
    {
        for (unsigned i = 0; i < NumStaticBufs; i++)
        {
            if (static_buffers_[i].getKey() == key)
            {
                return static_buffers_ + i;
            }
        }
        return NULL;
    }

    DynamicTransferBufferManagerEntry* findFirstDynamic(const TransferBufferManagerKey& key)
    {
        DynamicTransferBufferManagerEntry* dyn = dynamic_buffers_.get();
        while (dyn)
        {
            assert(!dyn->isEmpty());
            if (dyn->getKey() == key)
            {
                return dyn;
            }
            dyn = dyn->getNextListNode();
        }
        return NULL;
    }

    void optimizeStorage()
    {
        while (!dynamic_buffers_.isEmpty())
        {
            StaticBufferType* const sb = findFirstStatic(TransferBufferManagerKey());
            if (sb == NULL)
            {
                break;
            }
            DynamicTransferBufferManagerEntry* dyn = dynamic_buffers_.get();
            assert(dyn);
            assert(!dyn->isEmpty());
            if (sb->migrateFrom(dyn))
            {
                assert(!dyn->isEmpty());
                UAVCAN_TRACE("TransferBufferManager", "Storage optimization: Migrated %s",
                             dyn->getKey().toString().c_str());
                dynamic_buffers_.remove(dyn);
                DynamicTransferBufferManagerEntry::destroy(dyn, allocator_);
            }
            else
            {
                /* Migration can fail if a dynamic buffer contains more data than a static buffer can accomodate.
                 * This should never happen during normal operation because dynamic buffers are limited in growth.
                 */
                UAVCAN_TRACE("TransferBufferManager", "Storage optimization: MIGRATION FAILURE %s MAXSIZE %u",
                             dyn->getKey().toString().c_str(), MaxBufSize);
                assert(0);
                sb->reset();
                break;
            }
        }
    }

public:
    TransferBufferManager(IAllocator& allocator)
        : allocator_(allocator)
    {
        StaticAssert<(MaxBufSize > 0)>::check();
        StaticAssert<(NumStaticBufs > 0)>::check();
    }

    ~TransferBufferManager()
    {
        DynamicTransferBufferManagerEntry* dyn = dynamic_buffers_.get();
        while (dyn)
        {
            DynamicTransferBufferManagerEntry* const next = dyn->getNextListNode();
            dynamic_buffers_.remove(dyn);
            DynamicTransferBufferManagerEntry::destroy(dyn, allocator_);
            dyn = next;
        }
    }

    ITransferBuffer* access(const TransferBufferManagerKey& key)
    {
        if (key.isEmpty())
        {
            assert(0);
            return NULL;
        }
        TransferBufferManagerEntry* tbme = findFirstStatic(key);
        if (tbme)
        {
            return tbme;
        }
        return findFirstDynamic(key);
    }

    ITransferBuffer* create(const TransferBufferManagerKey& key)
    {
        if (key.isEmpty())
        {
            assert(0);
            return NULL;
        }
        remove(key);

        TransferBufferManagerEntry* tbme = findFirstStatic(TransferBufferManagerKey());
        if (tbme == NULL)
        {
            DynamicTransferBufferManagerEntry* dyn =
                DynamicTransferBufferManagerEntry::instantiate(allocator_, MaxBufSize);
            tbme = dyn;
            if (dyn == NULL)
            {
                return NULL;     // Epic fail.
            }
            dynamic_buffers_.insert(dyn);
            UAVCAN_TRACE("TransferBufferManager", "Dynamic buffer created [st=%u, dyn=%u], %s",
                         getNumStaticBuffers(), getNumDynamicBuffers(), key.toString().c_str());
        }
        else
        {
            UAVCAN_TRACE("TransferBufferManager", "Static buffer created [st=%u, dyn=%u], %s",
                         getNumStaticBuffers(), getNumDynamicBuffers(), key.toString().c_str());
        }

        if (tbme)
        {
            assert(tbme->isEmpty());
            tbme->reset(key);
        }
        return tbme;
    }

    void remove(const TransferBufferManagerKey& key)
    {
        assert(!key.isEmpty());

        TransferBufferManagerEntry* const tbme = findFirstStatic(key);
        if (tbme)
        {
            UAVCAN_TRACE("TransferBufferManager", "Static buffer deleted, %s", key.toString().c_str());
            tbme->reset();
            optimizeStorage();
            return;
        }

        DynamicTransferBufferManagerEntry* dyn = findFirstDynamic(key);
        if (dyn)
        {
            UAVCAN_TRACE("TransferBufferManager", "Dynamic buffer deleted, %s", key.toString().c_str());
            dynamic_buffers_.remove(dyn);
            DynamicTransferBufferManagerEntry::destroy(dyn, allocator_);
        }
    }

    bool isEmpty() const { return (getNumStaticBuffers() == 0) && (getNumDynamicBuffers() == 0); }

    unsigned getNumDynamicBuffers() const { return dynamic_buffers_.getLength(); }

    unsigned getNumStaticBuffers() const
    {
        unsigned res = 0;
        for (unsigned i = 0; i < NumStaticBufs; i++)
        {
            if (!static_buffers_[i].isEmpty())
            {
                res++;
            }
        }
        return res;
    }
};

template <>
class UAVCAN_EXPORT TransferBufferManager<0, 0> : public ITransferBufferManager
{
public:
    TransferBufferManager() { }
    TransferBufferManager(IAllocator& allocator)
    {
        (void)allocator;
    }

    ITransferBuffer* access(const TransferBufferManagerKey& key)
    {
        (void)key;
        return NULL;
    }

    ITransferBuffer* create(const TransferBufferManagerKey& key)
    {
        (void)key;
        return NULL;
    }

    void remove(const TransferBufferManagerKey& key)
    {
        (void)key;
    }

    bool isEmpty() const { return true; }
};

}
