#include <iostream>
#include <memory>
#include <cstring>
#include <iomanip>
#include <random>
#include <algorithm>
#include <complex>
#include <stack>
#include <utility>
#include <cassert>

using Byte = uint8_t;

constexpr Byte START_BYTE_BINARY_BLOCK = {0x24};
constexpr auto ENDING_TEXT_BLOCK = std::array<Byte, 4 >{'\r', '\n', '\r', '\n'};
union BinSize {
    Byte bytes[4];
    std::uint32_t size;
};
constexpr size_t BINARY_HEADER_SIZE = sizeof(START_BYTE_BINARY_BLOCK) + sizeof(BinSize);

void print(const Byte *ptr, std::size_t size) noexcept {
    std::cout << "==== Block byte size " << size << " bytes ====" << std::endl;
    for (int i = 0; i < 16; ++i) {
        std::cout << std::setfill('0') << std::setw(2) << std::uppercase << std::hex << i << "|" << std::dec;
    }
    std::cout << std::endl;
    size_t pos = 0;
    while (pos < size) {
        const auto length = std::min(16, (int) (size - pos));
        for (int i = 0; i < length; ++i) {
            std::cout << std::setfill('0') << std::setw(2)
                      << std::uppercase << std::hex << static_cast<int>(ptr[pos + i]) << "|" << std::dec;
        }
        std::cout << std::endl;
        pos += length;
    }
}

struct IReceiver {
    virtual ~IReceiver() = default;

    virtual void Receive(const Byte *data, std::size_t size) = 0;
};

struct ICallback {
    virtual ~ICallback() = default;

    virtual void BinaryPacket(const Byte *data, std::size_t size) = 0;

    virtual void TextPacket(const Byte *data, std::size_t size) = 0;
};

// callback isn't owner of data. copy data for safety.
struct Callback : public ICallback {
    Callback() = default;

    ~Callback() override {
        std::cout << __func__ << std::endl;
    }

    Callback(const Callback &callback) = delete;

    Callback &operator=(const Callback &callback) = delete;

    void BinaryPacket(const Byte *data, std::size_t size) override {
        std::cout << std::endl;
        std::cout << "==== BinaryPacket ====" << std::endl;
        print(data, size);
        std::cout << std::endl;
        values.push(std::vector<Byte>(data, data + size));
    }

    void TextPacket(const Byte *data, std::size_t size) override {
        std::cout << std::endl;
        std::cout << "==== TextPacket ====" << std::endl;
        print(data, size);
        std::cout << std::endl;
        values.push(std::vector<Byte>(data, data + size));
    }

    std::stack<std::vector<Byte>> values;
};

struct Receiver : public IReceiver {
    explicit Receiver(std::shared_ptr<ICallback> callback_)
            : callback(std::move(callback_)) {
    }

    ~Receiver() override {
        std::cout << __func__ << ", buffer size=" << buffer.size() << std::endl;
    }

    Receiver(const Receiver &receiver) = delete;

    Receiver &operator=(const Receiver &receiver) = delete;

    void Receive(const Byte *data, std::size_t size) override {
        std::cout << __func__ << " data=" << std::hex << reinterpret_cast<long>(data)
                  << std::dec << ", size=" << size << std::endl;

        if (size == 0) {
            return;
        }

        if (!buffer.empty()) {
            // append to buffer
            std::copy(data, data + size, std::back_inserter(buffer));
            data = buffer.data();
            size = buffer.size();
        }

        const auto beginData = data;
        const auto endData = data + size;
        const auto *ptr = data;
        while (ptr < endData) {
            if (*ptr == START_BYTE_BINARY_BLOCK) {
                const size_t left = endData - ptr;
                if (left > BINARY_HEADER_SIZE) {
                    ptr += sizeof(START_BYTE_BINARY_BLOCK);
                    const auto payloadSize = __bswap_32(BinSize{ptr[0],
                                                                ptr[1],
                                                                ptr[2],
                                                                ptr[3]}.size);
                    ptr += sizeof(BinSize);
                    const size_t left = endData - ptr;
                    if (payloadSize <= left) {
                        callback->BinaryPacket(ptr, payloadSize);
                        ptr += payloadSize;
                        data = ptr;
                    } else {
                        ptr = data;
                        break;
                    }
                } else {
                    break;
                }
            } else {
                const auto it = std::search(ptr, endData,
                                            std::begin(ENDING_TEXT_BLOCK),
                                            std::end(ENDING_TEXT_BLOCK));
                if (it != endData) {
                    // text and separator in a block
                    const std::ptrdiff_t packetSize = it - ptr;
                    callback->TextPacket(ptr, packetSize);
                    ptr += packetSize + ENDING_TEXT_BLOCK.size();
                } else {
                    break;
                }
            }
        }

        const std::ptrdiff_t processedBytes = ptr - beginData;
        const std::size_t unprocessedBytes = size - processedBytes;
        if (!buffer.empty() && processedBytes > 0) {
            // remove processedBytes from buffer
            buffer.erase(buffer.begin(), buffer.begin() + processedBytes);
        } else if (buffer.empty() && unprocessedBytes > 0) {
            // unprocessed bytes not in buffer
            std::copy(ptr, ptr + unprocessedBytes, std::back_inserter(buffer));
        }
    }

private:
    std::shared_ptr<ICallback> callback;
    std::vector<Byte> buffer;
};

template<typename T>
std::size_t pack(T value, std::size_t pos, Byte * const ptr) noexcept {
    ptr[pos] = START_BYTE_BINARY_BLOCK;
    pos += sizeof(START_BYTE_BINARY_BLOCK);
    std::memcpy(ptr + pos,
            // to little-endian
                BinSize{.size = __bswap_32(sizeof(value))}.bytes,
                sizeof(BinSize));
    pos += sizeof(BinSize);
    std::memcpy(ptr + pos, &value, sizeof(value));
    pos += sizeof(value);
    return pos;
}

template<>
std::size_t pack<const char *>(const char *value, std::size_t pos, Byte * const ptr) noexcept {
    const std::size_t size = std::strlen(value);
    std::copy(value, value + size, ptr + pos);
    pos += size;
    std::copy(std::begin(ENDING_TEXT_BLOCK), std::end(ENDING_TEXT_BLOCK), ptr + pos);
    pos += ENDING_TEXT_BLOCK.size();
    return pos;
}

template<typename ...Ts>
auto pack(std::tuple<Ts...> &&values) noexcept {
    const std::size_t sizeBlock = std::apply([](auto &&... args) {
        std::size_t size = 0;
        ((
                [&size](auto &&value) {
                    using Type = typename std::decay<decltype(value)>::type;
                    static_assert(std::is_pod<Type>::value, "type is not a pod");
                    if constexpr (std::is_same<const char *, Type>::value) {
                        size += std::strlen(value) + ENDING_TEXT_BLOCK.size();
                    } else {
                        size += BINARY_HEADER_SIZE + sizeof(Type);
                    }
                }(args)
        ), ...);
        return size;
    }, values);

    auto block = std::vector<Byte>(sizeBlock);
    std::apply([&block](auto &&... args) {
        std::size_t pos = 0;
        ((
                [&pos, &block](auto &&value) {
                    pos = pack(value, pos, block.data());
                }(args)
        ), ...);
    }, values);
    return std::move(block);
}

struct NotPodType {
    virtual ~NotPodType() = default;
};

template<typename ValueType>
inline
bool isTopValueEqual(std::stack<std::vector<Byte>> values, ValueType value) {
    return !values.empty() && *reinterpret_cast<ValueType *>(values.top().data()) == value;
}

int main() {
    std::mt19937 mt(std::random_device{}());
    auto callback = std::make_shared<Callback>();
    auto receiver = std::make_unique<Receiver>(callback);

    // test sending mixed data types
    {
        auto block = pack(std::make_tuple(
                                  "456",
                                  0xA0B0C0D,
                                  2.72f,
                                  3.14,
                                  u'a',
//                                  std::make_pair(4, 2),
//                                  std::complex<int>(4, 2),
//                                  NotPodType(),
                                  42ui
                          )
        );
        print(block.data(), block.size());
        receiver->Receive(block.data(), block.size());
    }

    // test sending text packet
    {
        auto block = pack(std::make_tuple("123456", "789123456123456"));
        print(block.data(), block.size());
        receiver->Receive(block.data(), block.size());
    }

    // test sending packets of binaries
    {
        auto block = pack(std::make_tuple('a', 12345));
        print(block.data(), block.size());
        receiver->Receive(block.data(), block.size());
    }

    // test sending parted mixed data packets
    {
        const auto value1 = static_cast<long long>(123456789);
        const auto value2 = static_cast<long long>(987654321);
        const auto binBlock = pack(std::make_tuple(value1, value2));
        print(binBlock.data(), binBlock.size());
        std::size_t pos = 0;
        std::size_t part = binBlock.size() * 0.75;
        while (pos < binBlock.size()) {
            const std::size_t left = binBlock.size() - pos;
            const std::size_t blockSize = left > part ? part : left;
            auto block = std::vector<Byte>(binBlock.begin() + pos, binBlock.begin() + pos + blockSize);
            receiver->Receive(block.data(), block.size());
            pos += part;
            part = binBlock.size() - part;
        }
        assert(isTopValueEqual(callback->values, value2));
        callback->values.pop();
        assert(isTopValueEqual(callback->values, value1));
        callback->values.pop();
    }
    {
        const auto value = static_cast<long long>(123456789);
        const auto binBlock = pack(std::make_tuple(value));
        for (int i = 0; i < binBlock.size(); ++i) {
            print(binBlock.data(), binBlock.size());
            std::size_t pos = 0;
            std::size_t part = i + 1;
            while (pos < binBlock.size()) {
                const std::size_t left = binBlock.size() - pos;
                const std::size_t blockSize = left > part ? part : left;
                auto block = std::vector<Byte>(binBlock.begin() + pos, binBlock.begin() + pos + blockSize);
                receiver->Receive(block.data(), block.size());
                pos += part;
                part = left;
            }
            assert(isTopValueEqual(callback->values, value));
            callback->values.pop();
        }
    }

    return 0;
}
