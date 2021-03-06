#include <Common/config.h>
#if USE_PROTOBUF

#include <AggregateFunctions/IAggregateFunction.h>
#include <boost/numeric/conversion/cast.hpp>
#include <DataTypes/DataTypesDecimal.h>
#include <Formats/ProtobufReader.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromVector.h>
#include <IO/WriteHelpers.h>
#include <optional>


namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_PROTOBUF_FORMAT;
    extern const int PROTOBUF_BAD_CAST;
}


namespace
{
    enum WireType
    {
        VARINT = 0,
        BITS64 = 1,
        LENGTH_DELIMITED = 2,
        GROUP_START = 3,
        GROUP_END = 4,
        BITS32 = 5,
    };

    // The following inequation should be always true to simplify conditions:
    // REACHED_END < any cursor position < min(END_OF_VARINT, END_OF_GROUP)
    constexpr UInt64 END_OF_VARINT = static_cast<UInt64>(-1);
    constexpr UInt64 END_OF_GROUP = static_cast<UInt64>(-2);

    Int64 decodeZigZag(UInt64 n) { return static_cast<Int64>((n >> 1) ^ (~(n & 1) + 1)); }

    void unknownFormat()
    {
        throw Exception("Protobuf messages are corrupted or doesn't match the provided schema", ErrorCodes::UNKNOWN_PROTOBUF_FORMAT);
    }
}



ProtobufReader::SimpleReader::SimpleReader(ReadBuffer & in_)
    : in(in_)
    , cursor(1 /* Should be greater than REACHED_END to simplify conditions */)
    , current_message_end(REACHED_END)
    , field_end(REACHED_END)
{
}

bool ProtobufReader::SimpleReader::startMessage()
{
    if ((current_message_end == REACHED_END) && parent_message_ends.empty())
    {
        // Start reading a root message.
        if (in.eof())
            return false;
        size_t size_of_message = readVarint();
        current_message_end = cursor + size_of_message;
    }
    else
    {
        // Start reading a nested message which is located inside a length-delimited field
        // of another message.s
        parent_message_ends.emplace_back(current_message_end);
        current_message_end = field_end;
    }
    field_end = REACHED_END;
    return true;
}

void ProtobufReader::SimpleReader::endMessage()
{
    if (current_message_end != REACHED_END)
    {
        if (current_message_end == END_OF_GROUP)
            ignoreGroup();
        else if (cursor < current_message_end)
            ignore(current_message_end - cursor);
        else if (cursor > current_message_end)
        {
            if (!parent_message_ends.empty())
                unknownFormat();
            moveCursorBackward(cursor - current_message_end);
        }
        current_message_end = REACHED_END;
    }

    field_end = REACHED_END;
    if (!parent_message_ends.empty())
    {
        current_message_end = parent_message_ends.back();
        parent_message_ends.pop_back();
    }
}

void ProtobufReader::SimpleReader::endRootMessage()
{
    UInt64 message_end = parent_message_ends.empty() ? current_message_end : parent_message_ends.front();
    if (message_end != REACHED_END)
    {
        if (cursor < message_end)
            ignore(message_end - cursor);
        else if (cursor > message_end)
            moveCursorBackward(cursor - message_end);
    }
    parent_message_ends.clear();
    current_message_end = REACHED_END;
    field_end = REACHED_END;
}

bool ProtobufReader::SimpleReader::readFieldNumber(UInt32 & field_number)
{
    if (field_end != REACHED_END)
    {
        if (field_end == END_OF_VARINT)
            ignoreVarint();
        else if (field_end == END_OF_GROUP)
            ignoreGroup();
        else if (cursor < field_end)
            ignore(field_end - cursor);
        field_end = REACHED_END;
    }

    if (cursor >= current_message_end)
    {
        current_message_end = REACHED_END;
        return false;
    }

    UInt64 varint = readVarint();
    if (varint & (static_cast<UInt64>(0xFFFFFFFF) << 32))
        unknownFormat();
    UInt32 key = static_cast<UInt32>(varint);
    field_number = (key >> 3);
    WireType wire_type = static_cast<WireType>(key & 0x07);
    switch (wire_type)
    {
        case BITS64:
        {
            field_end = cursor + 8;
            return true;
        }
        case LENGTH_DELIMITED:
        {
            size_t length = readVarint();
            field_end = cursor + length;
            return true;
        }
        case VARINT:
        {
            field_end = END_OF_VARINT;
            return true;
        }
        case GROUP_START:
        {
            field_end = END_OF_GROUP;
            return true;
        }
        case GROUP_END:
        {
            if (current_message_end != END_OF_GROUP)
                unknownFormat();
            current_message_end = REACHED_END;
            return false;
        }
        case BITS32:
        {
            field_end = cursor + 4;
            return true;
        }
    }
    unknownFormat();
    __builtin_unreachable();
}

bool ProtobufReader::SimpleReader::readUInt(UInt64 & value)
{
    if (cursor >= field_end)
    {
        field_end = REACHED_END;
        return false;
    }
    value = readVarint();
    if ((field_end == END_OF_VARINT) || (cursor >= field_end))
        field_end = REACHED_END;
    return true;
}

bool ProtobufReader::SimpleReader::readInt(Int64 & value)
{
    UInt64 varint;
    if (!readUInt(varint))
        return false;
    value = static_cast<Int64>(varint);
    return true;
}

bool ProtobufReader::SimpleReader::readSInt(Int64 & value)
{
    UInt64 varint;
    if (!readUInt(varint))
        return false;
    value = decodeZigZag(varint);
    return true;
}

template<typename T>
bool ProtobufReader::SimpleReader::readFixed(T & value)
{
    if (cursor >= field_end)
    {
        field_end = REACHED_END;
        return false;
    }
    readBinary(&value, sizeof(T));
    if (cursor >= field_end)
        field_end = REACHED_END;
    return true;
}

bool ProtobufReader::SimpleReader::readStringInto(PaddedPODArray<UInt8> & str)
{
    if (cursor > field_end)
        return false;
    size_t length = field_end - cursor;
    size_t old_size = str.size();
    str.resize(old_size + length);
    readBinary(reinterpret_cast<char*>(str.data() + old_size), length);
    field_end = REACHED_END;
    return true;
}

void ProtobufReader::SimpleReader::readBinary(void* data, size_t size)
{
    in.readStrict(reinterpret_cast<char*>(data), size);
    cursor += size;
}

void ProtobufReader::SimpleReader::ignore(UInt64 num_bytes)
{
    in.ignore(num_bytes);
    cursor += num_bytes;
}

void ProtobufReader::SimpleReader::moveCursorBackward(UInt64 num_bytes)
{
    if (in.offset() < num_bytes)
        unknownFormat();
    in.position() -= num_bytes;
    cursor -= num_bytes;
}

UInt64 ProtobufReader::SimpleReader::readVarint()
{
    char c;
    UInt64 result = 0;

#define PROTOBUF_READER_VARINT_READ_HELPER(i) \
    in.readStrict(&c, 1); \
    result |= static_cast<UInt64>(c) << (7 * i); \
    if constexpr (i < 9) \
    { \
        if (!(c & 0x80)) \
        { \
            cursor += i + 1; \
            return result; \
        } \
        if constexpr (i < 8) \
            result &= ((static_cast<UInt64>(0x80) << (7 * i)) - 1); \
    } \
    else \
    { \
        if (c == 1) \
        { \
            cursor += i + 1; \
            return result; \
        } \
    }
    PROTOBUF_READER_VARINT_READ_HELPER(0);
    PROTOBUF_READER_VARINT_READ_HELPER(1);
    PROTOBUF_READER_VARINT_READ_HELPER(2);
    PROTOBUF_READER_VARINT_READ_HELPER(3);
    PROTOBUF_READER_VARINT_READ_HELPER(4);
    PROTOBUF_READER_VARINT_READ_HELPER(5);
    PROTOBUF_READER_VARINT_READ_HELPER(6);
    PROTOBUF_READER_VARINT_READ_HELPER(7);
    PROTOBUF_READER_VARINT_READ_HELPER(8);
    PROTOBUF_READER_VARINT_READ_HELPER(9);
#undef PROTOBUF_READER_VARINT_READ_HELPER

    unknownFormat();
    return 0;
}

void ProtobufReader::SimpleReader::ignoreVarint()
{
    char c;

#define PROTOBUF_READER_VARINT_IGNORE_HELPER(i) \
    in.readStrict(&c, 1); \
    if constexpr (i < 9) \
    { \
        if (!(c & 0x80)) \
        { \
            cursor += i + 1; \
            return; \
        } \
    } \
    else \
    { \
        if (c == 1) \
        { \
            cursor += i + 1; \
            return; \
        } \
    }
    PROTOBUF_READER_VARINT_IGNORE_HELPER(0);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(1);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(2);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(3);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(4);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(5);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(6);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(7);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(8);
    PROTOBUF_READER_VARINT_IGNORE_HELPER(9);
#undef PROTOBUF_READER_VARINT_IGNORE_HELPER

    unknownFormat();
}

void ProtobufReader::SimpleReader::ignoreGroup()
{
    size_t level = 1;
    while (true)
    {
        UInt64 varint = readVarint();
        WireType wire_type = static_cast<WireType>(varint & 0x07);
        switch (wire_type)
        {
            case VARINT:
            {
                ignoreVarint();
                break;
            }
            case BITS64:
            {
                ignore(8);
                break;
            }
            case LENGTH_DELIMITED:
            {
                ignore(readVarint());
                break;
            }
            case GROUP_START:
            {
                ++level;
                break;
            }
            case GROUP_END:
            {
                if (!--level)
                    return;
                break;
            }
            case BITS32:
            {
                ignore(4);
                break;
            }
        }
        unknownFormat();
    }
}


class ProtobufReader::ConverterBaseImpl : public ProtobufReader::IConverter
{
public:
    ConverterBaseImpl(SimpleReader & simple_reader_, const google::protobuf::FieldDescriptor * field_)
        : simple_reader(simple_reader_), field(field_) {}

    bool readStringInto(PaddedPODArray<UInt8> &) override
    {
        cannotConvertType("String");
        return false;
    }

    bool readInt8(Int8 &) override
    {
        cannotConvertType("Int8");
        return false;
    }

    bool readUInt8(UInt8 &) override
    {
        cannotConvertType("UInt8");
        return false;
    }

    bool readInt16(Int16 &) override
    {
        cannotConvertType("Int16");
        return false;
    }

    bool readUInt16(UInt16 &) override
    {
        cannotConvertType("UInt16");
        return false;
    }

    bool readInt32(Int32 &) override
    {
        cannotConvertType("Int32");
        return false;
    }

    bool readUInt32(UInt32 &) override
    {
        cannotConvertType("UInt32");
        return false;
    }

    bool readInt64(Int64 &) override
    {
        cannotConvertType("Int64");
        return false;
    }

    bool readUInt64(UInt64 &) override
    {
        cannotConvertType("UInt64");
        return false;
    }

    bool readUInt128(UInt128 &) override
    {
        cannotConvertType("UInt128");
        return false;
    }

    bool readFloat32(Float32 &) override
    {
        cannotConvertType("Float32");
        return false;
    }

    bool readFloat64(Float64 &) override
    {
        cannotConvertType("Float64");
        return false;
    }

    void prepareEnumMapping8(const std::vector<std::pair<std::string, Int8>> &) override {}
    void prepareEnumMapping16(const std::vector<std::pair<std::string, Int16>> &) override {}

    bool readEnum8(Int8 &) override
    {
        cannotConvertType("Enum");
        return false;
    }

    bool readEnum16(Int16 &) override
    {
        cannotConvertType("Enum");
        return false;
    }

    bool readUUID(UUID &) override
    {
        cannotConvertType("UUID");
        return false;
    }

    bool readDate(DayNum &) override
    {
        cannotConvertType("Date");
        return false;
    }

    bool readDateTime(time_t &) override
    {
        cannotConvertType("DateTime");
        return false;
    }

    bool readDecimal32(Decimal32 &, UInt32, UInt32) override
    {
        cannotConvertType("Decimal32");
        return false;
    }

    bool readDecimal64(Decimal64 &, UInt32, UInt32) override
    {
        cannotConvertType("Decimal64");
        return false;
    }

    bool readDecimal128(Decimal128 &, UInt32, UInt32) override
    {
        cannotConvertType("Decimal128");
        return false;
    }

    bool readAggregateFunction(const AggregateFunctionPtr &, AggregateDataPtr, Arena &) override
    {
        cannotConvertType("AggregateFunction");
        return false;
    }

protected:
    void cannotConvertType(const String & type_name)
    {
        throw Exception(
            String("Could not convert type '") + field->type_name() + "' from protobuf field '" + field->name() + "' to data type '"
                + type_name + "'",
            ErrorCodes::PROTOBUF_BAD_CAST);
    }

    void cannotConvertValue(const String & value, const String & type_name)
    {
        throw Exception(
            "Could not convert value '" + value + "' from protobuf field '" + field->name() + "' to data type '" + type_name + "'",
            ErrorCodes::PROTOBUF_BAD_CAST);
    }

    template <typename To, typename From>
    To numericCast(From value)
    {
        if constexpr (std::is_same_v<To, From>)
            return value;
        To result;
        try
        {
            result = boost::numeric_cast<To>(value);
        }
        catch (boost::numeric::bad_numeric_cast &)
        {
            cannotConvertValue(toString(value), TypeName<To>::get());
        }
        return result;
    }

    template <typename To>
    To parseFromString(const PaddedPODArray<UInt8> & str)
    {
        try
        {
            To result;
            ReadBufferFromString buf(str);
            readText(result, buf);
            return result;
        }
        catch (...)
        {
            cannotConvertValue(StringRef(str.data(), str.size()).toString(), TypeName<To>::get());
            __builtin_unreachable();
        }
    }

    SimpleReader & simple_reader;
    const google::protobuf::FieldDescriptor * field;
};



class ProtobufReader::ConverterFromString : public ConverterBaseImpl
{
public:
    using ConverterBaseImpl::ConverterBaseImpl;

    bool readStringInto(PaddedPODArray<UInt8> & str) override { return simple_reader.readStringInto(str); }

    bool readInt8(Int8 & value) override { return readNumeric(value); }
    bool readUInt8(UInt8 & value) override { return readNumeric(value); }
    bool readInt16(Int16 & value) override { return readNumeric(value); }
    bool readUInt16(UInt16 & value) override { return readNumeric(value); }
    bool readInt32(Int32 & value) override { return readNumeric(value); }
    bool readUInt32(UInt32 & value) override { return readNumeric(value); }
    bool readInt64(Int64 & value) override { return readNumeric(value); }
    bool readUInt64(UInt64 & value) override { return readNumeric(value); }
    bool readFloat32(Float32 & value) override { return readNumeric(value); }
    bool readFloat64(Float64 & value) override { return readNumeric(value); }

    void prepareEnumMapping8(const std::vector<std::pair<String, Int8>> & name_value_pairs) override
    {
        prepareEnumNameToValueMap(name_value_pairs);
    }
    void prepareEnumMapping16(const std::vector<std::pair<String, Int16>> & name_value_pairs) override
    {
        prepareEnumNameToValueMap(name_value_pairs);
    }

    bool readEnum8(Int8 & value) override { return readEnum(value); }
    bool readEnum16(Int16 & value) override { return readEnum(value); }

    bool readUUID(UUID & uuid) override
    {
        if (!readTempString())
            return false;
        ReadBufferFromString buf(temp_string);
        readUUIDText(uuid, buf);
        return true;
    }

    bool readDate(DayNum & date) override
    {
        if (!readTempString())
            return false;
        ReadBufferFromString buf(temp_string);
        readDateText(date, buf);
        return true;
    }

    bool readDateTime(time_t & tm) override
    {
        if (!readTempString())
            return false;
        ReadBufferFromString buf(temp_string);
        readDateTimeText(tm, buf);
        return true;
    }

    bool readDecimal32(Decimal32 & decimal, UInt32 precision, UInt32 scale) override { return readDecimal(decimal, precision, scale); }
    bool readDecimal64(Decimal64 & decimal, UInt32 precision, UInt32 scale) override { return readDecimal(decimal, precision, scale); }
    bool readDecimal128(Decimal128 & decimal, UInt32 precision, UInt32 scale) override { return readDecimal(decimal, precision, scale); }

    bool readAggregateFunction(const AggregateFunctionPtr & function, AggregateDataPtr place, Arena & arena) override
    {
        if (!readTempString())
            return false;
        ReadBufferFromString buf(temp_string);
        function->deserialize(place, buf, &arena);
        return true;
    }

private:
    bool readTempString()
    {
        temp_string.clear();
        return simple_reader.readStringInto(temp_string);
    }

    template <typename T>
    bool readNumeric(T & value)
    {
        if (!readTempString())
            return false;
        value = parseFromString<T>(temp_string);
        return true;
    }

    template<typename T>
    bool readEnum(T & value)
    {
        if (!readTempString())
            return false;
        StringRef strref = StringRef(temp_string.data(), temp_string.size());
        auto it = enum_name_to_value_map->find(strref);
        if (it == enum_name_to_value_map->end())
            cannotConvertValue(strref.toString(), "Enum");
        value = static_cast<T>(it->second);
        return true;
    }

    template <typename T>
    bool readDecimal(Decimal<T> & decimal, UInt32 precision, UInt32 scale)
    {
        if (!readTempString())
            return false;
        ReadBufferFromString buf(temp_string);
        DataTypeDecimal<Decimal<T>>::readText(decimal, buf, precision, scale);
        return true;
    }

    template <typename T>
    void prepareEnumNameToValueMap(const std::vector<std::pair<String, T>> & name_value_pairs)
    {
        if (enum_name_to_value_map.has_value())
            return;
        enum_name_to_value_map.emplace();
        for (const auto & name_value_pair : name_value_pairs)
            enum_name_to_value_map->emplace(name_value_pair.first, name_value_pair.second);
    }

    PaddedPODArray<UInt8> temp_string;
    std::optional<std::unordered_map<StringRef, Int16>> enum_name_to_value_map;
};

#define PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_STRINGS(field_type_id) \
    template<> \
    class ProtobufReader::ConverterImpl<field_type_id> : public ConverterFromString \
    { \
        using ConverterFromString::ConverterFromString; \
    }
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_STRINGS(google::protobuf::FieldDescriptor::TYPE_STRING);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_STRINGS(google::protobuf::FieldDescriptor::TYPE_BYTES);
#undef PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_STRINGS



template <int field_type_id, typename T>
class ProtobufReader::ConverterFromNumber : public ConverterBaseImpl
{
public:
    using ConverterBaseImpl::ConverterBaseImpl;

    bool readStringInto(PaddedPODArray<UInt8> & str) override
    {
        T number;
        if (!readField(number))
            return false;
        WriteBufferFromVector<PaddedPODArray<UInt8>> buf(str);
        writeText(number, buf);
        return true;
    }

    bool readInt8(Int8 & value) override { return readNumeric(value); }
    bool readUInt8(UInt8 & value) override { return readNumeric(value); }
    bool readInt16(Int16 & value) override { return readNumeric(value); }
    bool readUInt16(UInt16 & value) override { return readNumeric(value); }
    bool readInt32(Int32 & value) override { return readNumeric(value); }
    bool readUInt32(UInt32 & value) override { return readNumeric(value); }
    bool readInt64(Int64 & value) override { return readNumeric(value); }
    bool readUInt64(UInt64 & value) override { return readNumeric(value); }
    bool readFloat32(Float32 & value) override { return readNumeric(value); }
    bool readFloat64(Float64 & value) override { return readNumeric(value); }

    bool readEnum8(Int8 & value) override { return readEnum(value); }
    bool readEnum16(Int16 & value) override { return readEnum(value); }

    void prepareEnumMapping8(const std::vector<std::pair<String, Int8>> & name_value_pairs) override
    {
        prepareSetOfEnumValues(name_value_pairs);
    }
    void prepareEnumMapping16(const std::vector<std::pair<String, Int16>> & name_value_pairs) override
    {
        prepareSetOfEnumValues(name_value_pairs);
    }

    bool readDate(DayNum & date) override
    {
        UInt16 number;
        if (!readNumeric(number))
            return false;
        date = DayNum(number);
        return true;
    }

    bool readDateTime(time_t & tm) override
    {
        UInt32 number;
        if (!readNumeric(number))
            return false;
        tm = number;
        return true;
    }

    bool readDecimal32(Decimal32 & decimal, UInt32, UInt32 scale) override { return readDecimal(decimal, scale); }
    bool readDecimal64(Decimal64 & decimal, UInt32, UInt32 scale) override { return readDecimal(decimal, scale); }
    bool readDecimal128(Decimal128 & decimal, UInt32, UInt32 scale) override { return readDecimal(decimal, scale); }

private:
    template <typename To>
    bool readNumeric(To & value)
    {
        T number;
        if (!readField(number))
            return false;
        value = numericCast<To>(number);
        return true;
    }

    template<typename EnumType>
    bool readEnum(EnumType & value)
    {
        if constexpr (!std::is_integral_v<T>)
            cannotConvertType("Enum"); // It's not correct to convert floating point to enum.
        T number;
        if (!readField(number))
            return false;
        value = numericCast<EnumType>(number);
         if (set_of_enum_values->find(value) == set_of_enum_values->end())
            cannotConvertValue(toString(value), "Enum");
        return true;
    }

    template<typename EnumType>
    void prepareSetOfEnumValues(const std::vector<std::pair<String, EnumType>> & name_value_pairs)
    {
        if (set_of_enum_values.has_value())
            return;
        set_of_enum_values.emplace();
        for (const auto & name_value_pair : name_value_pairs)
            set_of_enum_values->emplace(name_value_pair.second);
    }

    template <typename S>
    bool readDecimal(Decimal<S> & decimal, UInt32 scale)
    {
        T number;
        if (!readField(number))
            return false;
        decimal.value = convertToDecimal<DataTypeNumber<T>, DataTypeDecimal<Decimal<S>>>(number, scale);
        return true;
    }

    bool readField(T & value)
    {
        if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_INT32) && std::is_same_v<T, Int64>)
            return simple_reader.readInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_SINT32) && std::is_same_v<T, Int64>)
            return simple_reader.readSInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_UINT32) && std::is_same_v<T, UInt64>)
            return simple_reader.readUInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_INT64) && std::is_same_v<T, Int64>)
            return simple_reader.readInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_SINT64) && std::is_same_v<T, Int64>)
            return simple_reader.readSInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_UINT64) && std::is_same_v<T, UInt64>)
            return simple_reader.readUInt(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_FIXED32) && std::is_same_v<T, UInt32>)
            return simple_reader.readFixed(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_SFIXED32) && std::is_same_v<T, Int32>)
            return simple_reader.readFixed(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_FIXED64) && std::is_same_v<T, UInt64>)
            return simple_reader.readFixed(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_SFIXED64) && std::is_same_v<T, Int64>)
            return simple_reader.readFixed(value);
        else if constexpr ((field_type_id == google::protobuf::FieldDescriptor::TYPE_FLOAT) && std::is_same_v<T, float>)
            return simple_reader.readFixed(value);
        else
        {
            static_assert((field_type_id == google::protobuf::FieldDescriptor::TYPE_DOUBLE) && std::is_same_v<T, double>);
            return simple_reader.readFixed(value);
        }
    }

    std::optional<std::unordered_set<Int16>> set_of_enum_values;
};

#define PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(field_type_id, field_type) \
    template<> \
    class ProtobufReader::ConverterImpl<field_type_id> : public ConverterFromNumber<field_type_id, field_type> \
    { \
        using ConverterFromNumber::ConverterFromNumber; \
    }
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_INT32, Int64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_SINT32, Int64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_UINT32, UInt64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_INT64, Int64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_SINT64, Int64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_UINT64, UInt64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_FIXED32, UInt32);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_SFIXED32, Int32);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_FIXED64, UInt64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_SFIXED64, Int64);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_FLOAT, float);
PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS(google::protobuf::FieldDescriptor::TYPE_DOUBLE, double);
#undef PROTOBUF_READER_CONVERTER_IMPL_SPECIALIZATION_FOR_NUMBERS



template<>
class ProtobufReader::ConverterImpl<google::protobuf::FieldDescriptor::TYPE_BOOL> : public ConverterBaseImpl
{
public:
    using ConverterBaseImpl::ConverterBaseImpl;

    bool readStringInto(PaddedPODArray<UInt8> & str) override
    {
        bool b;
        if (!readField(b))
            return false;
        WriteBufferFromVector<PaddedPODArray<UInt8>> buf(str);
        writeString(StringRef(b ? "true" : "false"), buf);
        return true;
    }

    bool readInt8(Int8 & value) override { return readNumeric(value); }
    bool readUInt8(UInt8 & value) override { return readNumeric(value); }
    bool readInt16(Int16 & value) override { return readNumeric(value); }
    bool readUInt16(UInt16 & value) override { return readNumeric(value); }
    bool readInt32(Int32 & value) override { return readNumeric(value); }
    bool readUInt32(UInt32 & value) override { return readNumeric(value); }
    bool readInt64(Int64 & value) override { return readNumeric(value); }
    bool readUInt64(UInt64 & value) override { return readNumeric(value); }
    bool readFloat32(Float32 & value) override { return readNumeric(value); }
    bool readFloat64(Float64 & value) override { return readNumeric(value); }
    bool readDecimal32(Decimal32 & decimal, UInt32, UInt32) override { return readNumeric(decimal.value); }
    bool readDecimal64(Decimal64 & decimal, UInt32, UInt32) override { return readNumeric(decimal.value); }
    bool readDecimal128(Decimal128 & decimal, UInt32, UInt32) override { return readNumeric(decimal.value); }

private:
    template<typename T>
    bool readNumeric(T & value)
    {
        bool b;
        if (!readField(b))
            return false;
        value = b ? 1 : 0;
        return true;
    }

    bool readField(bool & b)
    {
        UInt64 number;
        if (!simple_reader.readUInt(number))
            return false;
        b = static_cast<bool>(number);
        return true;
    }
};



template<>
class ProtobufReader::ConverterImpl<google::protobuf::FieldDescriptor::TYPE_ENUM> : public ConverterBaseImpl
{
public:
    using ConverterBaseImpl::ConverterBaseImpl;

    bool readStringInto(PaddedPODArray<UInt8> & str) override
    {
        prepareEnumPbNumberToNameMap();
        Int64 pbnumber;
        if (!readField(pbnumber))
            return false;
        auto it = enum_pbnumber_to_name_map->find(pbnumber);
        if (it == enum_pbnumber_to_name_map->end())
            cannotConvertValue(toString(pbnumber), "Enum");

        WriteBufferFromVector<PaddedPODArray<UInt8>> buf(str);
        writeString(it->second, buf);
        return true;
    }

    bool readInt8(Int8 & value) override { return readNumeric(value); }
    bool readUInt8(UInt8 & value) override { return readNumeric(value); }
    bool readInt16(Int16 & value) override { return readNumeric(value); }
    bool readUInt16(UInt16 & value) override { return readNumeric(value); }
    bool readInt32(Int32 & value) override { return readNumeric(value); }
    bool readUInt32(UInt32 & value) override { return readNumeric(value); }
    bool readInt64(Int64 & value) override { return readNumeric(value); }
    bool readUInt64(UInt64 & value) override { return readNumeric(value); }

    void prepareEnumMapping8(const std::vector<std::pair<String, Int8>> & name_value_pairs) override
    {
        prepareEnumPbNumberToValueMap(name_value_pairs);
    }
    void prepareEnumMapping16(const std::vector<std::pair<String, Int16>> & name_value_pairs) override
    {
        prepareEnumPbNumberToValueMap(name_value_pairs);
    }

    bool readEnum8(Int8 & value) override { return readEnum(value); }
    bool readEnum16(Int16 & value) override { return readEnum(value); }

private:
    template <typename T>
    bool readNumeric(T & value)
    {
        Int64 pbnumber;
        if (!readField(pbnumber))
            return false;
        value = numericCast<T>(pbnumber);
        return true;
    }

    template<typename T>
    bool readEnum(T & value)
    {
        Int64 pbnumber;
        if (!readField(pbnumber))
            return false;
        auto it = enum_pbnumber_to_value_map->find(pbnumber);
        if (it == enum_pbnumber_to_value_map->end())
            cannotConvertValue(toString(pbnumber), "Enum");
        value = static_cast<T>(it->second);
        return true;
    }

    void prepareEnumPbNumberToNameMap()
    {
        if (enum_pbnumber_to_name_map.has_value())
            return;
        enum_pbnumber_to_name_map.emplace();
        const auto * enum_type = field->enum_type();
        for (int i = 0; i != enum_type->value_count(); ++i)
        {
            const auto * enum_value = enum_type->value(i);
            enum_pbnumber_to_name_map->emplace(enum_value->number(), enum_value->name());
        }
    }

    template <typename T>
    void prepareEnumPbNumberToValueMap(const std::vector<std::pair<String, T>> & name_value_pairs)
    {
        if (enum_pbnumber_to_value_map.has_value())
            return;
        enum_pbnumber_to_value_map.emplace();
        for (const auto & name_value_pair : name_value_pairs)
        {
            Int16 value = name_value_pair.second;
            const auto * enum_descriptor = field->enum_type()->FindValueByName(name_value_pair.first);
            if (enum_descriptor)
                enum_pbnumber_to_value_map->emplace(enum_descriptor->number(), value);
        }
    }

    bool readField(Int64 & enum_pbnumber)
    {
        return simple_reader.readInt(enum_pbnumber);
    }

    std::optional<std::unordered_map<Int64, StringRef>> enum_pbnumber_to_name_map;
    std::optional<std::unordered_map<Int64, Int16>> enum_pbnumber_to_value_map;
};


ProtobufReader::ProtobufReader(
    ReadBuffer & in_, const google::protobuf::Descriptor * message_type, const std::vector<String> & column_names)
    : simple_reader(in_)
{
    root_message = ProtobufColumnMatcher::matchColumns<ColumnMatcherTraits>(column_names, message_type);
    setTraitsDataAfterMatchingColumns(root_message.get());
}

ProtobufReader::~ProtobufReader() = default;

void ProtobufReader::setTraitsDataAfterMatchingColumns(Message * message)
{
    for (Field & field : message->fields)
    {
        if (field.nested_message)
        {
            setTraitsDataAfterMatchingColumns(field.nested_message.get());
            continue;
        }
        switch (field.field_descriptor->type())
        {
#define PROTOBUF_READER_CONVERTER_CREATING_CASE(field_type_id) \
            case field_type_id: \
                field.data.converter = std::make_unique<ConverterImpl<field_type_id>>(simple_reader, field.field_descriptor); \
                break
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_STRING);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_BYTES);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_INT32);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_SINT32);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_UINT32);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_FIXED32);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_SFIXED32);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_INT64);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_SINT64);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_UINT64);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_FIXED64);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_SFIXED64);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_FLOAT);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_DOUBLE);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_BOOL);
            PROTOBUF_READER_CONVERTER_CREATING_CASE(google::protobuf::FieldDescriptor::TYPE_ENUM);
#undef PROTOBUF_READER_CONVERTER_CREATING_CASE
            default: __builtin_unreachable();
        }
        message->data.field_number_to_field_map.emplace(field.field_number, &field);
    }
}

bool ProtobufReader::startMessage()
{
    if (!simple_reader.startMessage())
        return false;
    current_message = root_message.get();
    current_field_index = 0;
    return true;
}

void ProtobufReader::endMessage()
{
    simple_reader.endRootMessage();
    current_message = nullptr;
    current_converter = nullptr;
}

bool ProtobufReader::readColumnIndex(size_t & column_index)
{
    while (true)
    {
        UInt32 field_number;
        if (!simple_reader.readFieldNumber(field_number))
        {
            if (!current_message->parent)
            {
                current_converter = nullptr;
                return false;
            }
            simple_reader.endMessage();
            current_field_index = current_message->index_in_parent;
            current_message = current_message->parent;
            continue;
        }

        const Field * field = nullptr;
        for (; current_field_index < current_message->fields.size(); ++current_field_index)
        {
            const Field & f = current_message->fields[current_field_index];
            if (f.field_number == field_number)
            {
                field = &f;
                break;
            }
            if (f.field_number > field_number)
                break;
        }

        if (!field)
        {
            const auto & field_number_to_field_map = current_message->data.field_number_to_field_map;
            auto it = field_number_to_field_map.find(field_number);
            if (it == field_number_to_field_map.end())
                continue;
            field = it->second;
        }

        if (field->nested_message)
        {
            simple_reader.startMessage();
            current_message = field->nested_message.get();
            current_field_index = 0;
            continue;
        }

        column_index = field->column_index;
        current_converter = field->data.converter.get();
        return true;
    }
}

}
#endif
