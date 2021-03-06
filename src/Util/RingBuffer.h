﻿/*
 * MIT License
 *
 * Copyright (c) 2016-2019 xiongziliang <771730766@qq.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef UTIL_RINGBUFFER_H_
#define UTIL_RINGBUFFER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <functional>
#include <deque>
#include "Poller/EventPoller.h"

using namespace std;

//自适应环形缓存大小的取值范围
#define RING_MIN_SIZE 1
#define LOCK_GUARD(mtx) lock_guard<decltype(mtx)> lck(mtx)

namespace toolkit {

template<typename T>
class RingDelegate {
public:
    typedef std::shared_ptr<RingDelegate> Ptr;
    RingDelegate() {}
    virtual ~RingDelegate() {}
    virtual void onWrite(const T &in, bool is_key = true) = 0;
};

template<typename T>
class _RingStorage;

template<typename T>
class _RingStorageInternal;

template<typename T>
class _RingReaderDispatcher;

/**
* 环形缓存读取器
* 该对象的事件触发都会在绑定的poller线程中执行
* 所以把锁去掉了
* 对该对象的一切操作都应该在poller线程中执行
* @tparam T
*/
template<typename T>
class _RingReader {
public:
    typedef std::shared_ptr<_RingReader> Ptr;
    friend class _RingReaderDispatcher<T>;

    _RingReader(const std::shared_ptr<_RingStorage<T> > &storage, bool use_cache) {
        _storage = storage;
        _use_cache = use_cache;
    }

    ~_RingReader() {}

    void setReadCB(const function<void(const T &)> &cb) {
        if (!cb) {
            _read_cb = [](const T &) {};
        } else {
            _ignored_count = 0;
            _start_on_read = false;
            _read_cb = cb;
            flushGop();
        }
    }

    void setDetachCB(const function<void()> &cb) {
        if (!cb) {
            _detach_cb = []() {};
        } else {
            _detach_cb = cb;
        }
    }

private:
    void onRead(const T &data, bool is_key) {
        if (_start_on_read || !_use_cache) {
            //已经获取到关键帧或者不要求第一帧是关键帧
            _read_cb(data);
            return;
        }

        if (!_start_on_read && is_key) {
            //尚未获取到关键帧，并且此帧是关键帧
            _start_on_read = true;
            _read_cb(data);
            return;
        }

        if(++_ignored_count >= _storage->maxSize()){
            //忽略了太多的数据，强制开始触发onRead回调
            _start_on_read = true;
        }
    }

    void onDetach() const {
        _detach_cb();
    }

    void flushGop() {
        if (!_use_cache) {
            return;
        }
        auto &cache = _storage->getCache();
        for (auto &pr : cache) {
            onRead(pr.second, pr.first);
        }
    }

private:
    function<void(const T &)> _read_cb = [](const T &) {};
    function<void(void)> _detach_cb = []() {};
    shared_ptr<_RingStorage<T> > _storage;
    bool _use_cache;
    bool _start_on_read = false;
    int _ignored_count = 0;
};

template<typename T>
class _RingStorageInternal {
public:
    typedef std::shared_ptr<_RingStorageInternal> Ptr;

    _RingStorageInternal(int max_size) {
        _max_size = max_size;
    }

    ~_RingStorageInternal() {}


    /**
     * 写入环形缓存数据
     * @param in 数据
     * @param is_key 是否为关键帧
     * @return 是否触发重置环形缓存大小
     */
    inline void write(const T &in, bool is_key = true) {
        if (is_key) {
            _data_cache.clear();
        }
        _data_cache.emplace_back(std::make_pair(is_key, std::move(in)));
        if (_data_cache.size() > _max_size) {
            _data_cache.pop_front();
        }
    }

    Ptr clone() const {
        Ptr ret(new _RingStorageInternal());
        ret->_data_cache = _data_cache;
        ret->_max_size = _max_size;
        return ret;
    }

    const deque<pair<bool, T> > &getCache() const {
        return _data_cache;
    }

    int maxSize() const{
        return _max_size;
    }
private:
    _RingStorageInternal() = default;

private:
    deque<pair<bool, T> > _data_cache;
    int _max_size;
};

template<typename T>
class _RingStorage {
public:
    typedef std::shared_ptr<_RingStorage> Ptr;

    _RingStorage(int size, int max_size) {
        if (size <= 0) {
            size = max_size;
            _can_resize = true;
            _max_size = max_size;
        }

        _storage_internal = std::make_shared<_RingStorageInternal<T> >(size);
    }

    ~_RingStorage() {}


    /**
     * 写入环形缓存数据
     * @param in 数据
     * @param is_key 是否为关键帧
     * @return 是否触发重置环形缓存大小
     */
    inline void write(const T &in, bool is_key = true) {
        computeGopSize(is_key);
        _storage_internal->write(in, is_key);
    }

    const deque<pair<bool, T> > &getCache() const {
        return _storage_internal->getCache();
    }

    int maxSize() const {
        return _storage_internal->maxSize();
    }

    Ptr clone() const {
        Ptr ret(new _RingStorage());
        ret->_beset_size = _beset_size;
        ret->_storage_internal = _storage_internal->clone();
        ret->_total_count = _total_count;
        ret->_last_key_count = _last_key_count;
        ret->_can_resize = _can_resize;
        ret->_max_size = _max_size;
        return ret;
    }

private:
    _RingStorage() = default;

    inline bool computeGopSize(bool is_key) {
        if (!_can_resize || _beset_size) {
            return false;
        }
        _total_count++;
        if (!is_key) {
            return false;
        }
        //关键帧
        if (!_last_key_count) {
            //第一次获取关键帧
            _last_key_count = _total_count;
            return false;
        }

        //第二次获取关键帧，计算两个I帧之间的包个数
        //缓存最多2个GOP，确保缓存中最少有一个GOP
        _beset_size = (_total_count - _last_key_count) * 2;
        if (_beset_size > _max_size) {
            _beset_size = _max_size;
        }
        if (_beset_size < RING_MIN_SIZE) {
            _beset_size = RING_MIN_SIZE;
        }
        _storage_internal = std::make_shared<_RingStorageInternal<T> >(_beset_size);
        return true;
    }

private:
    typename _RingStorageInternal<T>::Ptr _storage_internal;
    //计算最佳环形缓存大小的参数
    int _beset_size = 0;
    int _total_count = 0;
    int _last_key_count = 0;
    bool _can_resize = false;
    int _max_size;
};

template<typename T>
class RingBuffer;

/**
* 环形缓存事件派发器，只能一个poller线程操作它
* @tparam T
*/
template<typename T>
class _RingReaderDispatcher : public enable_shared_from_this<_RingReaderDispatcher<T> > {
public:
    typedef std::shared_ptr<_RingReaderDispatcher> Ptr;
    typedef _RingReader<T> RingReader;
    typedef _RingStorage<T> RingStorage;

    friend class RingBuffer<T>;

    ~_RingReaderDispatcher() {
        decltype(_reader_map) reader_map;
        reader_map.swap(_reader_map);
        for (auto &pr : reader_map) {
            auto reader = pr.second.lock();
            if (reader) {
                reader->onDetach();
            }
        }
    }

private:
    _RingReaderDispatcher(const typename RingStorage::Ptr &storage, const function<void(int, bool)> &onSizeChanged) {
        _storage = storage;
        _reader_size = 0;
        _on_size_changed = onSizeChanged;
    }

    void write(const T &in, bool is_key = true) {
        for (auto it = _reader_map.begin(); it != _reader_map.end();) {
            auto reader = it->second.lock();
            if (!reader) {
                it = _reader_map.erase(it);
                --_reader_size;
                onSizeChanged(false);
                continue;
            }
            reader->onRead(in, is_key);
            ++it;
        }
        _storage->write(in, is_key);
    }

    std::shared_ptr<RingReader> attach(const EventPoller::Ptr &poller, bool use_cache) {
        if (!poller->isCurrentThread()) {
            throw std::runtime_error("必须在绑定的poller线程中执行attach操作");
        }

        weak_ptr<_RingReaderDispatcher> weakSelf = this->shared_from_this();
        auto on_dealloc = [weakSelf, poller](RingReader *ptr) {
            poller->async([weakSelf, ptr]() {
                auto strongSelf = weakSelf.lock();
                if (strongSelf && strongSelf->_reader_map.erase(ptr)) {
                    --strongSelf->_reader_size;
                    strongSelf->onSizeChanged(false);
                }
                delete ptr;
            });
        };

        std::shared_ptr<RingReader> reader(new RingReader(_storage, use_cache), on_dealloc);
        _reader_map[reader.get()] = std::move(reader);
        ++_reader_size;
        onSizeChanged(true);
        return reader;
    }

    int readerCount() {
        return _reader_size;
    }

    void onSizeChanged(bool add_flag) {
        _on_size_changed(_reader_size, add_flag);
    }

private:
    function<void(int, bool)> _on_size_changed;
    atomic_int _reader_size;
    typename RingStorage::Ptr _storage;
    unordered_map<void *, std::weak_ptr<RingReader> > _reader_map;
};

template<typename T>
class RingBuffer : public enable_shared_from_this<RingBuffer<T> > {
public:
    typedef std::shared_ptr<RingBuffer> Ptr;
    typedef _RingReader<T> RingReader;
    typedef _RingStorage<T> RingStorage;
    typedef _RingReaderDispatcher<T> RingReaderDispatcher;
    typedef function<void(const EventPoller::Ptr &poller, int size, bool add_flag)> onReaderChanged;

    RingBuffer(int size = 0, int max_size = 1024, const onReaderChanged &cb = nullptr) {
        _on_reader_changed = cb;
        _storage = std::make_shared<RingStorage>(size, max_size);
    }

    ~RingBuffer() {}

    void write(const T &in, bool is_key = true) {
        if (_delegate) {
            _delegate->onWrite(in, is_key);
            return;
        }

        LOCK_GUARD(_mtx_map);
        _storage->write(in, is_key);
        for (auto &pr : _dispatcher_map) {
            auto &second = pr.second;
            pr.first->async([second, in, is_key]() {
                second->write(in, is_key);
            }, false);
        }
    }

    void setDelegate(const typename RingDelegate<T>::Ptr &delegate) {
        _delegate = delegate;
    }

    std::shared_ptr<RingReader> attach(const EventPoller::Ptr &poller, bool use_cache = true) {
        typename RingReaderDispatcher::Ptr dispatcher;
        {
            LOCK_GUARD(_mtx_map);
            auto &ref = _dispatcher_map[poller];
            if (!ref) {
                weak_ptr<RingBuffer> weakSelf = this->shared_from_this();
                auto onSizeChanged = [weakSelf, poller](int size, bool add_flag) {
                    auto strongSelf = weakSelf.lock();
                    if (!strongSelf) {
                        return;
                    }
                    strongSelf->onSizeChanged(poller, size, add_flag);
                };

                auto onDealloc = [poller](RingReaderDispatcher *ptr) {
                    poller->async([ptr]() {
                        delete ptr;
                    });
                };
                ref.reset(new RingReaderDispatcher(_storage->clone(), std::move(onSizeChanged)), std::move(onDealloc));
            }
            dispatcher = ref;
        }

        return dispatcher->attach(poller, use_cache);
    }

    int readerCount() {
        LOCK_GUARD(_mtx_map);
        int total = 0;
        for (auto &pr : _dispatcher_map) {
            total += pr.second->readerCount();
        }
        return total;
    }

private:
    void onSizeChanged(const EventPoller::Ptr &poller, int size, bool add_flag) {
        if (size == 0) {
            LOCK_GUARD(_mtx_map);
            _dispatcher_map.erase(poller);
        }

        if (_on_reader_changed) {
            _on_reader_changed(poller, size, add_flag);
        }
    }

private:
    struct HashOfPtr {
        std::size_t operator()(const EventPoller::Ptr &key) const {
            return (uint64_t) key.get();
        }
    };

private:
    mutex _mtx_map;
    typename RingStorage::Ptr _storage;
    typename RingDelegate<T>::Ptr _delegate;
    onReaderChanged _on_reader_changed;
    unordered_map<EventPoller::Ptr, typename RingReaderDispatcher::Ptr, HashOfPtr> _dispatcher_map;
};

}; /* namespace toolkit */

#endif /* UTIL_RINGBUFFER_H_ */
