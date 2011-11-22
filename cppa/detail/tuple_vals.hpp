#ifndef TUPLE_VALS_HPP
#define TUPLE_VALS_HPP

#include <stdexcept>

#include "cppa/util/type_list.hpp"

#include "cppa/detail/tdata.hpp"
#include "cppa/detail/abstract_tuple.hpp"
#include "cppa/detail/serialize_tuple.hpp"

namespace cppa { namespace detail {

template<typename... ElementTypes>
class tuple_vals : public abstract_tuple
{

    typedef abstract_tuple super;

    typedef tdata<ElementTypes...> data_type;

    typedef util::type_list<ElementTypes...> element_types;

    data_type m_data;

    element_types m_types;

    template<typename... Types>
    void* tdata_mutable_at(tdata<Types...>& d, size_t pos)
    {
        return (pos == 0) ? &(d.head) : tdata_mutable_at(d.tail(), pos - 1);
    }

    template<typename... Types>
    const void* tdata_at(const tdata<Types...>& d, size_t pos) const
    {
        return (pos == 0) ? &(d.head) : tdata_at(d.tail(), pos - 1);
    }

 public:

    tuple_vals(const tuple_vals& other) : super(), m_data(other.m_data) { }

    tuple_vals() : m_data() { }

    tuple_vals(const ElementTypes&... args) : m_data(args...) { }

    inline const data_type& data() const { return m_data; }

    inline data_type& data_ref() { return m_data; }

    void* mutable_at(size_t pos)
    {
        return tdata_mutable_at(m_data, pos);
    }

    size_t size() const
    {
        return sizeof...(ElementTypes);
    }

    tuple_vals* copy() const
    {
        return new tuple_vals(*this);
    }

    const void* at(size_t pos) const
    {
        return tdata_at(m_data, pos);
    }

    const uniform_type_info& utype_info_at(size_t pos) const
    {
        return m_types.at(pos);
    }

    bool equals(const abstract_tuple& other) const
    {
        if (size() != other.size()) return false;
        const tuple_vals* o = dynamic_cast<const tuple_vals*>(&other);
        if (o)
        {
            return m_data == (o->m_data);
        }
        return abstract_tuple::equals(other);
    }

};

} } // namespace cppa::detail

#endif // TUPLE_VALS_HPP
