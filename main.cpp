#include <iostream>

#include <variant>
#include <set>
#include <mutex>
#include <thread>
#include <queue>
#include <compare>

//template <typename T>
//concept

template <typename Price>
class MarketOrderWhenUnfilled {
public:
    enum Action {
        FORGET, STORE
    };

    Action action;
    Price price;

    MarketOrderWhenUnfilled(const Action& behavior, const Price& price): action(behavior), price(price) {}
    MarketOrderWhenUnfilled(): MarketOrderWhenUnfilled(FORGET, 0) {}
};

template <typename Quantity, typename Timestamp>
class Order {
public:
    Quantity quantity;
    Timestamp timestamp;
protected:
    Order(const Quantity& quantity, const Timestamp& timestamp): quantity(quantity), timestamp(timestamp) {}
};

template <typename Price, typename Quantity, typename Timestamp>
class MarketOrder: public Order<Quantity, Timestamp> {
public:
    MarketOrderWhenUnfilled<Price> onUnfilled;

protected:
    MarketOrder(const MarketOrderWhenUnfilled<Price>& onUnfilled, const Quantity& quantity, const Timestamp& timestamp): Order<Quantity, Timestamp>(quantity, timestamp), onUnfilled(onUnfilled) {};
};

template <typename Price, typename Quantity, typename Timestamp>
class BuyMarketOrder: public MarketOrder<Price, Quantity, Timestamp> {
public:
    BuyMarketOrder(const MarketOrderWhenUnfilled<Price>& onUnfilled, const Quantity& quantity, const Timestamp& timestamp): MarketOrder<Price, Quantity, Timestamp>(onUnfilled, quantity, timestamp) {};

    friend std::ostream& operator << (std::ostream& out, const BuyMarketOrder<Price, Quantity, Timestamp>& order) {
        out << "Market(buy; quantity=" << order.quantity << ", timestamp= " << order.timestamp << ")";
        return out;
    }
};

template <typename Price, typename Quantity, typename Timestamp>
class SellMarketOrder: public MarketOrder<Price, Quantity, Timestamp> {
public:
    SellMarketOrder(const MarketOrderWhenUnfilled<Price>& onUnfilled, const Quantity& quantity, const Timestamp& timestamp): MarketOrder<Price, Quantity, Timestamp>(onUnfilled, quantity, timestamp) {};

    friend std::ostream& operator << (std::ostream& out, const SellMarketOrder<Price, Quantity, Timestamp>& order) {
        out << "Market(sell; quantity=" << order.quantity << ", timestamp= " << order.timestamp << ")";
        return out;
    }
};

template <typename Price, typename Quantity, typename Timestamp>
class LimitOrder: public Order<Quantity, Timestamp> {
public:
    Price price;
protected:
    LimitOrder(const Price& price, const Quantity& quantity, const Timestamp& timestamp): Order<Quantity, Timestamp>(quantity, timestamp), price(price) {}
};

template <typename Price, typename Quantity, typename Timestamp>
class BuyLimitOrder: public LimitOrder<Price, Quantity, Timestamp> {
public:
    BuyLimitOrder(const Price& price, const Quantity& quantity, const Timestamp& timestamp): LimitOrder<Price, Quantity, Timestamp>(price, quantity, timestamp) {}

    friend std::ostream& operator << (std::ostream& out, const BuyLimitOrder<Price, Quantity, Timestamp>& order) {
        out << "Limit(buy; price=" << order.price << ", quantity=" << order.quantity << ", timestamp= " << order.timestamp << ")";
        return out;
    }
};

template <typename Price, typename Quantity, typename Timestamp>
class SellLimitOrder: public LimitOrder<Price, Quantity, Timestamp> {
public:
    SellLimitOrder(const Price& price, const Quantity& quantity, const Timestamp& timestamp): LimitOrder<Price, Quantity, Timestamp>(price, quantity, timestamp) {}

    friend std::ostream& operator << (std::ostream& out, const SellLimitOrder<Price, Quantity, Timestamp>& order) {
        out << "Limit(sell; price=" << order.price << ", quantity=" << order.quantity << ", timestamp= " << order.timestamp << ")";
        return out;
    }
};

template <typename OrderID, typename Order>
struct ReceivedOrder {
    OrderID id;
    Order order;

    ReceivedOrder() = default;
    ReceivedOrder(const OrderID& id, const Order& order): id(id), order(order) {}

    friend std::ostream& operator << (std::ostream& out, const ReceivedOrder<OrderID, Order>& order) {
        out << "Order#" << order.id << "{" << order.order << "}";
        return out;
    }
};


template <typename Price, typename Quantity, typename Timestamp>
class OrderBook {
public:
    using OrderID = unsigned;

    using BuyMarketOrder = BuyMarketOrder<Price, Quantity, Timestamp>;
    using SellMarketOrder = SellMarketOrder<Price, Quantity, Timestamp>;
    using BuyLimitOrder = BuyLimitOrder<Price, Quantity, Timestamp>;
    using SellLimitOrder = SellLimitOrder<Price, Quantity, Timestamp>;

    using QueuedOrder = std::variant<
        ReceivedOrder<OrderID, BuyMarketOrder>,
        ReceivedOrder<OrderID, SellMarketOrder>,
        ReceivedOrder<OrderID, BuyLimitOrder>,
        ReceivedOrder<OrderID, SellLimitOrder>
    >;

    struct PooledOrder {
        OrderID id;
        Price price;
        Quantity quantity;

        PooledOrder(const OrderID& id, const Price& price, const Quantity& quantity): id(id), price(price), quantity(quantity) {}

        friend std::strong_ordering operator <=> (const PooledOrder& self, const PooledOrder& other) {
            return self.price <=> other.price;
        }

        // FIXME: remove
        // the following should not be necessary, but it doesn't compile without it
        // (at least as of time of writing)
        friend bool operator < (const PooledOrder& self, const PooledOrder& other) {
            return (self <=> other) < 0;
        }

        friend bool operator > (const PooledOrder& self, const PooledOrder& other) {
            return (self <=> other) > 0;
        }
    };

#define define_execute_overload_for(order_type) \
    OrderID execute(const order_type& order) { \
        OrderID id = nextID(); \
        std::lock_guard<std::mutex> guard(queue_mutex); \
        queue.push(ReceivedOrder<OrderID, order_type>(id, order)); \
        return id; \
    }

    define_execute_overload_for(BuyMarketOrder)
    define_execute_overload_for(SellMarketOrder)
    define_execute_overload_for(BuyLimitOrder)
    define_execute_overload_for(SellLimitOrder)

#undef define_execute_overload_for

    OrderBook(): orderID(), executeVisitor(ExecuteVisitor(*this)) {
        const int num_executors = 1;

        for(int i = 0; i < num_executors; i++){
            executor_threads.emplace_back(&OrderBook<Price, Quantity, Timestamp>::executor, this);
        }
    };

    ~OrderBook() {
        {
            std::lock_guard<std::mutex> guard(queue_mutex);
            queue.push(ReceivedOrder<OrderID, BuyMarketOrder>(0, BuyMarketOrder(MarketOrderWhenUnfilled<Price>(), Quantity(), Timestamp())));
        }

        for(auto& executor_thread : executor_threads){
            executor_thread.join();
        }

        std::cout << asks.size() << ' ' << bids.size() << std::endl;
    }

private:
    OrderID orderID;

    std::mutex queue_mutex;
    std::vector<std::thread> executor_threads;
    std::queue<QueuedOrder> queue;

    std::mutex asks_mutex;
    std::multiset<PooledOrder, std::less<>> asks;

    std::mutex bids_mutex;
    std::multiset<PooledOrder, std::greater<>> bids;

    void executor() {
        std::chrono::milliseconds sleep_duration(1);

        OrderID lastExecutedID;

        do {
            // Check if queue is empty and if so don't bother locking the queue
            if(queue.empty()) {
                std::this_thread::sleep_for(sleep_duration);
            }

            // Get the next order and execute it
            std::lock_guard<std::mutex> guard(queue_mutex);

            // recheck now that we own the queue
            if(queue.empty()) {
                continue;
            }

            const QueuedOrder& order = queue.front();
            queue.pop();

            lastExecutedID = std::visit(executeVisitor, order);
        } while(lastExecutedID);
    }

    OrderID nextID() {
        return ++orderID;
    }

    // std::visit, simple implementation
    class ExecuteVisitor {
        OrderBook<Price, Quantity, Timestamp>& order_book;
    public:
        explicit ExecuteVisitor(OrderBook<Price, Quantity, Timestamp>& order_book): order_book(order_book) {}

        OrderID operator()(const ReceivedOrder<OrderID, BuyMarketOrder> &order) const {
            std::cout << "Executing " << order << std::endl;

            std::lock_guard<std::mutex> guard(order_book.asks_mutex);

            auto it = order_book.asks.begin();
            Quantity unfilled = order.order.quantity;
            Price paid {};

            while(unfilled > 0 && it != order_book.asks.end()) {
                if(it->quantity >= unfilled) {
                    paid += unfilled * it->price;
                    auto new_ask = *it;
                    new_ask.quantity -= unfilled;
                    it = order_book.asks.erase(it);
                    order_book.asks.insert(new_ask);
                    unfilled = 0;
                } else {
                    paid += it->quantity * it->price;
                    unfilled -= it->quantity;
                    it = order_book.asks.erase(it);
                }
            }

            if(unfilled) {
                std::lock_guard<std::mutex> bids_guard(order_book.bids_mutex);

                switch(order.order.onUnfilled.action) {
                    case MarketOrderWhenUnfilled<Price>::FORGET:
                        std::cerr << "forgetting unfilled bid for " << unfilled << std::endl;
                        break;
                    case MarketOrderWhenUnfilled<Price>::STORE:
                        std::cerr << "storing unfilled bid for " << unfilled << " at " << order.order.onUnfilled.price << std::endl;
                        order_book.bids.insert(PooledOrder(order.id, order.order.onUnfilled.price, order.order.quantity));
                        break;
                }
            }

            return order.id;
        }

        OrderID operator()(const ReceivedOrder<OrderID, SellMarketOrder> &order) const {
            std::cout << "Executing " << order << std::endl;

            std::lock_guard<std::mutex> bids_guard(order_book.bids_mutex);

            auto it = order_book.bids.begin();
            Quantity unfilled = order.order.quantity;
            Price paid {};

            while(unfilled > 0 && it != order_book.bids.end()) {
                if(it->quantity >= unfilled) {
                    paid += unfilled * it->price;
                    auto new_bid = *it;
                    new_bid.quantity -= unfilled;
                    it = order_book.bids.erase(it);
                    order_book.bids.insert(new_bid);
                    unfilled = 0;
                } else {
                    paid += it->quantity * it->price;
                    unfilled -= it->quantity;
                    it = order_book.bids.erase(it);
                }
            }

            if(unfilled) {
                std::lock_guard<std::mutex> asks_guard(order_book.asks_mutex);

                switch(order.order.onUnfilled.action) {
                    case MarketOrderWhenUnfilled<Price>::FORGET:
                        std::cerr << "forgetting unfilled ask for " << unfilled << std::endl;
                        break;
                    case MarketOrderWhenUnfilled<Price>::STORE:
                        std::cerr << "storing unfilled ask for " << unfilled << " at " << order.order.onUnfilled.price << std::endl;
                        order_book.asks.insert(PooledOrder(order.id, order.order.onUnfilled.price, order.order.quantity));
                        break;
                }
            }
        }

        OrderID operator()(const ReceivedOrder<OrderID, BuyLimitOrder> &order) const {
            return order.id;
        }

        OrderID operator()(const ReceivedOrder<OrderID, SellLimitOrder> &order) const {
            return order.id;
        }
    };

    ExecuteVisitor executeVisitor;
};


int main() {
    using Price = long;
    using Quantity = long;
    using Timestamp = long;


    OrderBook<Quantity, Price, Timestamp> book;
    BuyMarketOrder<Price, Quantity, Timestamp> buy {MarketOrderWhenUnfilled<Price>(MarketOrderWhenUnfilled<Price>::STORE, 10), 4, 23};
    long buy_id = book.execute(buy);
    SellMarketOrder<Price, Quantity, Timestamp> sell {MarketOrderWhenUnfilled<Price>(MarketOrderWhenUnfilled<Price>::STORE, 10), 4, 23};
    long sell_id = book.execute(sell);

    return 0;
}
