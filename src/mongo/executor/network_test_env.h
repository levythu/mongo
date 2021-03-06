/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <type_traits>
#include <vector>

#include "mongo/db/repl/replication_executor.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class BSONObj;
class CatalogManagerReplicaSet;
class DistLockManagerMock;
class ShardRegistry;
template <typename T>
class StatusWith;

namespace executor {

/**
 * A network infrastructure for testing.
 */
class NetworkTestEnv {
public:
    /**
     * Wraps a std::future but will cancel any pending network operations in its destructor if
     * the future wasn't successfully waited on in the main test thread.
     * Without this behavior any operations launched asynchronously might never terminate if they
     * are waiting for network operations to complete.
     */
    template <class T>
    class FutureHandle {
    public:
        FutureHandle<T>(stdx::future<T> future,
                        executor::TaskExecutor* executor,
                        executor::NetworkInterfaceMock* network)
            : _future(std::move(future)), _executor(executor), _network(network) {}

#if defined(_MSC_VER) && _MSC_VER < 1900  // MVSC++ <= 2013 can't generate default move operations
        FutureHandle(FutureHandle&& other)
            : _future(std::move(other._future)),
              _executor(other._executor),
              _network(other._network) {
            other._executor = nullptr;
            other._network = nullptr;
        }

        FutureHandle& operator=(FutureHandle&& other) {
            _future = std::move(other._future);
            _executor = other._executor;
            _network = other._network;

            other._executor = nullptr;
            other._network = nullptr;

            return *this;
        }
#else
        FutureHandle(FutureHandle&& other) = default;
        FutureHandle& operator=(FutureHandle&& other) = default;
#endif

        ~FutureHandle() {
            if (_future.valid()) {
                _network->exitNetwork();
                _executor->shutdown();
                _future.wait();
            }
        }

        template <class Rep, class Period>
        T timed_get(const stdx::chrono::duration<Rep, Period>& timeout_duration) {
            auto status = _future.wait_for(timeout_duration);
            ASSERT(status == stdx::future_status::ready);

            return _future.get();
        }

    private:
        stdx::future<T> _future;
        executor::TaskExecutor* _executor;
        executor::NetworkInterfaceMock* _network;
    };

    /**
     * Helper method for launching an asynchronous task in a way that will guarantees that the
     * task will finish even if the task depends on network traffic via the mock network and there's
     * an exception that prevents the main test thread from scheduling responses to the network
     * operations.  It does this by returning a FutureHandle that wraps std::future and cancels
     * all pending network operations in its destructor.
     * Must be defined in the header because of its use of templates.
     */
    template <typename Lambda>
    FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(Lambda&& func) const {
        auto future = async(stdx::launch::async, std::forward<Lambda>(func));
        return NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type>{
            std::move(future), _executor, _mockNetwork};
    }


    using OnCommandFunction = stdx::function<StatusWith<BSONObj>(const RemoteCommandRequest&)>;

    using OnFindCommandFunction =
        stdx::function<StatusWith<std::vector<BSONObj>>(const RemoteCommandRequest&)>;

    /**
     * Create a new environment based on the given network.
     */
    NetworkTestEnv(repl::ReplicationExecutor* executor, NetworkInterfaceMock* network);

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(OnCommandFunction func);
    void onFindCommand(OnFindCommandFunction func);

private:
    // Task executor used for running asynchronous operations.
    repl::ReplicationExecutor* _executor;

    // Mocked out network under the task executor.
    NetworkInterfaceMock* _mockNetwork;
};

}  // namespace executor
}  // namespace mongo
