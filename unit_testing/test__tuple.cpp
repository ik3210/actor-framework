#include <list>
#include <array>
#include <string>
#include <utility>
#include <iostream>
#include <typeinfo>
#include <functional>
#include <type_traits>

#include "test.hpp"

#include "cppa/on.hpp"
#include "cppa/cow_tuple.hpp"
#include "cppa/pattern.hpp"
#include "cppa/any_tuple.hpp"
#include "cppa/to_string.hpp"
#include "cppa/tuple_cast.hpp"
#include "cppa/intrusive_ptr.hpp"
#include "cppa/tpartial_function.hpp"
#include "cppa/uniform_type_info.hpp"

#include "cppa/util/rm_option.hpp"
#include "cppa/util/purge_refs.hpp"
#include "cppa/util/deduce_ref_type.hpp"

#include "cppa/detail/matches.hpp"
#include "cppa/detail/invokable.hpp"
#include "cppa/detail/projection.hpp"
#include "cppa/detail/types_array.hpp"
#include "cppa/detail/value_guard.hpp"
#include "cppa/detail/object_array.hpp"

#include <boost/progress.hpp>

using std::cout;
using std::endl;

using namespace cppa;
using namespace cppa::detail;

template<typename... T>
struct pseudo_tuple
{
    typedef void* ptr_type;
    typedef void const* const_ptr_type;

    ptr_type data[sizeof...(T) > 0 ? sizeof...(T) : 1];

    inline const_ptr_type at(size_t p) const
    {
        return data[p];
    }

    inline ptr_type mutable_at(size_t p)
    {
        return const_cast<ptr_type>(data[p]);
    }

    inline void*& operator[](size_t p)
    {
        return data[p];
    }
};

template<class List>
struct pseudo_tuple_from_type_list;

template<typename... Ts>
struct pseudo_tuple_from_type_list<util::type_list<Ts...> >
{
    typedef pseudo_tuple<Ts...> type;
};

template<size_t N, typename... Tn>
typename util::at<N, Tn...>::type const& get(pseudo_tuple<Tn...> const& tv)
{
    static_assert(N < sizeof...(Tn), "N >= tv.size()");
    return *reinterpret_cast<typename util::at<N, Tn...>::type const*>(tv.at(N));
}

template<size_t N, typename... Tn>
typename util::at<N, Tn...>::type& get_ref(pseudo_tuple<Tn...>& tv)
{
    static_assert(N < sizeof...(Tn), "N >= tv.size()");
    return *reinterpret_cast<typename util::at<N, Tn...>::type*>(tv.mutable_at(N));
}

// covers wildcard_position::multiple and wildcard_position::in_between
template<wildcard_position, class Pattern, class FilteredPattern>
struct invoke_policy_impl
{
    typedef FilteredPattern filtered_pattern;

    template<class Tuple>
    static bool can_invoke(std::type_info const& type_token,
                           Tuple const& tup)
    {
        typedef typename match_impl_from_type_list<Tuple, Pattern>::type mimpl;
        return type_token == typeid(filtered_pattern) ||  mimpl::_(tup);
    }

    template<class Target, typename PtrType, class Tuple>
    static bool invoke(Target& target,
                       std::type_info const& type_token,
                       detail::tuple_impl_info,
                       PtrType*,
                       Tuple& tup)
    {
        typedef typename match_impl_from_type_list<
                    typename std::remove_const<Tuple>::type,
                    Pattern
                >::type
                mimpl;

        util::fixed_vector<size_t, filtered_pattern::size> mv;
        if (type_token == typeid(filtered_pattern) ||  mimpl::_(tup, mv))
        {
            typedef typename pseudo_tuple_from_type_list<filtered_pattern>::type
                    ttup_type;
            ttup_type ttup;
            // if we strip const here ...
            for (size_t i = 0; i < filtered_pattern::size; ++i)
            {
                ttup[i] = const_cast<void*>(tup.at(mv[i]));
            }
            // ... we restore it here again
            typedef typename util::if_else<
                        std::is_const<Tuple>,
                        ttup_type const&,
                        util::wrapped<ttup_type&>
                    >::type
                    ttup_ref;
            ttup_ref ttup_fwd = ttup;
            return util::unchecked_apply_tuple<bool>(target, ttup_fwd);
        }
        return false;
    }
};

template<class Pattern, typename... Ts>
struct invoke_policy_impl<wildcard_position::nil,
                          Pattern, util::type_list<Ts...> >
{
    typedef util::type_list<Ts...> filtered_pattern;

    typedef detail::tdata<Ts...> native_data_type;

    typedef typename detail::static_types_array<Ts...> arr_type;

    template<class Target, class Tup>
    static bool invoke(std::integral_constant<bool, false>, Target&, Tup&)
    {
        return false;
    }

    template<class Target, class Tup>
    static bool invoke(std::integral_constant<bool, true>,
                       Target& target, Tup& tup)
    {
        return util::unchecked_apply_tuple<bool>(target, tup);
    }

    template<class Target, typename PtrType, class Tuple>
    static bool invoke(Target& target,
                       std::type_info const&,
                       detail::tuple_impl_info,
                       PtrType*,
                       Tuple& tup,
                       typename util::disable_if<
                           std::is_same<typename std::remove_const<Tuple>::type,
                                        detail::abstract_tuple>
                       >::type* = 0)
    {
        static constexpr bool can_apply =
                    util::tl_binary_forall<
                        typename util::tl_map<
                            typename Tuple::types,
                            util::purge_refs
                        >::type,
                        filtered_pattern,
                        std::is_same
                    >::value;
        return invoke(std::integral_constant<bool, can_apply>{}, target, tup);
    }

    template<class Target, typename PtrType, typename Tuple>
    static bool invoke(Target& target,
                       std::type_info const& arg_types,
                       detail::tuple_impl_info timpl,
                       PtrType* native_arg,
                       Tuple& tup,
                       typename util::enable_if<
                           std::is_same<typename std::remove_const<Tuple>::type,
                                        detail::abstract_tuple>
                       >::type* = 0)
    {
        if (arg_types == typeid(filtered_pattern))
        {
            if (native_arg)
            {
                typedef typename util::if_else_c<
                            std::is_const<PtrType>::value,
                            native_data_type const*,
                            util::wrapped<native_data_type*>
                        >::type
                        cast_type;
                auto arg = reinterpret_cast<cast_type>(native_arg);
                return util::unchecked_apply_tuple<bool>(target, *arg);
            }
            // 'fall through'
        }
        else if (timpl == detail::dynamically_typed)
        {
            auto& arr = arr_type::arr;
            if (tup.size() != filtered_pattern::size)
            {
                return false;
            }
            for (size_t i = 0; i < filtered_pattern::size; ++i)
            {
                if (arr[i] != tup.type_at(i))
                {
                    return false;
                }
            }
            // 'fall through'
        }
        else
        {
            return false;
        }
        typedef pseudo_tuple<Ts...> ttup_type;
        ttup_type ttup;
        // if we strip const here ...
        for (size_t i = 0; i < sizeof...(Ts); ++i)
            ttup[i] = const_cast<void*>(tup.at(i));
        // ... we restore it here again
        typedef typename util::if_else<
                    std::is_const<PtrType>,
                    ttup_type const&,
                    util::wrapped<ttup_type&>
                >::type
                ttup_ref;
        ttup_ref ttup_fwd = ttup;
        return util::unchecked_apply_tuple<bool>(target, ttup_fwd);
    }

    template<class Tuple>
    static bool can_invoke(std::type_info const& arg_types, Tuple const&)
    {
        return arg_types == typeid(filtered_pattern);
    }
};

template<>
struct invoke_policy_impl<wildcard_position::leading,
                          util::type_list<anything>,
                          util::type_list<> >
{
    template<class Tuple>
    static inline bool can_invoke(std::type_info const&,
                                  Tuple const&)
    {
        return true;
    }

    template<class Target, typename... Args>
    static bool invoke(Target& target, Args&&...)
    {
        return target();
    }
};

template<class Pattern, typename... Ts>
struct invoke_policy_impl<wildcard_position::trailing,
                          Pattern, util::type_list<Ts...> >
{
    typedef util::type_list<Ts...> filtered_pattern;

    template<class Tuple>
    static bool can_invoke(std::type_info const& arg_types,
                           Tuple const& tup)
    {
        if (arg_types == typeid(filtered_pattern))
        {
            return true;
        }
        typedef detail::static_types_array<Ts...> arr_type;
        auto& arr = arr_type::arr;
        if (tup.size() < filtered_pattern::size)
        {
            return false;
        }
        for (size_t i = 0; i < filtered_pattern::size; ++i)
        {
            if (arr[i] != tup.type_at(i))
            {
                return false;
            }
        }
        return true;
    }

    template<class Target, typename PtrType, class Tuple>
    static bool invoke(Target& target,
                       std::type_info const& arg_types,
                       detail::tuple_impl_info,
                       PtrType*,
                       Tuple& tup)
    {
        if (!can_invoke(arg_types, tup)) return false;
        typedef pseudo_tuple<Ts...> ttup_type;
        ttup_type ttup;
        for (size_t i = 0; i < sizeof...(Ts); ++i)
            ttup[i] = const_cast<void*>(tup.at(i));
        // ensure const-correctness
        typedef typename util::if_else<
                    std::is_const<Tuple>,
                    ttup_type const&,
                    util::wrapped<ttup_type&>
                >::type
                ttup_ref;
        ttup_ref ttup_fwd = ttup;
        return util::unchecked_apply_tuple<bool>(target, ttup_fwd);
    }

};

template<class Pattern, typename... Ts>
struct invoke_policy_impl<wildcard_position::leading,
                          Pattern, util::type_list<Ts...> >
{
    typedef util::type_list<Ts...> filtered_pattern;

    template<class Tuple>
    static bool can_invoke(std::type_info const& arg_types,
                           Tuple const& tup)
    {
        if (arg_types == typeid(filtered_pattern))
        {
            return true;
        }
        typedef detail::static_types_array<Ts...> arr_type;
        auto& arr = arr_type::arr;
        if (tup.size() < filtered_pattern::size)
        {
            return false;
        }
        size_t i = tup.size() - filtered_pattern::size;
        size_t j = 0;
        while (j < filtered_pattern::size)
        {
            if (arr[i++] != tup.type_at(j++))
            {
                return false;
            }
        }
        return true;
    }

    template<class Target, typename PtrType, class Tuple>
    static bool invoke(Target& target,
                       std::type_info const& arg_types,
                       detail::tuple_impl_info,
                       PtrType*,
                       Tuple& tup)
    {
        if (!can_invoke(arg_types, tup)) return false;
        typedef pseudo_tuple<Ts...> ttup_type;
        ttup_type ttup;
        size_t i = tup.size() - filtered_pattern::size;
        size_t j = 0;
        while (j < filtered_pattern::size)
        {
            ttup[j++] = const_cast<void*>(tup.at(i++));
        }
        // ensure const-correctness
        typedef typename util::if_else<
                    std::is_const<Tuple>,
                    ttup_type const&,
                    util::wrapped<ttup_type&>
                >::type
                ttup_ref;
        ttup_ref ttup_fwd = ttup;
        return util::unchecked_apply_tuple<bool>(target, ttup_fwd);
    }

};

template<class Pattern>
struct invoke_policy
        : invoke_policy_impl<
            get_wildcard_position<Pattern>(),
            Pattern,
            typename util::tl_filter_not_type<Pattern, anything>::type>
{
};


template<class Pattern, class Projection, class PartialFunction>
struct projection_partial_function_pair : std::pair<Projection, PartialFunction>
{
    template<typename... Args>
    projection_partial_function_pair(Args&&... args)
        : std::pair<Projection, PartialFunction>(std::forward<Args>(args)...)
    {
    }

    typedef Pattern pattern_type;
};

template<class Expr, class Guard, class Transformers, class Pattern>
struct get_cfl
{
    typedef typename util::get_callable_trait<Expr>::type ctrait;

    typedef typename util::tl_filter_not_type<
                Pattern,
                anything
            >::type
            filtered_pattern;

    typedef typename util::tl_pad_right<
                Transformers,
                filtered_pattern::size
            >::type
            padded_transformers;

    typedef typename util::tl_map<
                filtered_pattern,
                std::add_const,
                std::add_lvalue_reference
            >::type
            base_signature;

    typedef typename util::tl_map_conditional<
                typename util::tl_pad_left<
                    typename ctrait::arg_types,
                    filtered_pattern::size
                >::type,
                std::is_lvalue_reference,
                false,
                std::add_const,
                std::add_lvalue_reference
            >::type
            padded_expr_args;


    // override base signature with required argument types of Expr
    // and result types of transformation
    typedef typename util::tl_zip<
                typename util::tl_map<
                    padded_transformers,
                    util::get_result_type,
                    util::rm_option,
                    std::add_lvalue_reference
                >::type,
                typename util::tl_zip<
                    padded_expr_args,
                    base_signature,
                    util::left_or_right
                >::type,
                util::left_or_right
            >::type
            partial_fun_signature;

    // 'inherit' mutable references from partial_fun_signature
    // for arguments without transformation
    typedef typename util::tl_zip<
                typename util::tl_zip<
                    padded_transformers,
                    partial_fun_signature,
                    util::if_not_left
                >::type,
                base_signature,
                util::deduce_ref_type
            >::type
            projection_signature;

    typedef typename projection_from_type_list<
                padded_transformers,
                projection_signature
            >::type
            type1;

    typedef typename get_tpartial_function<
                Expr,
                Guard,
                partial_fun_signature
            >::type
            type2;

    typedef projection_partial_function_pair<Pattern, type1, type2> type;

};

template<typename First, typename Second>
struct pjf_same_pattern
        : std::is_same<typename First::second::pattern_type,
                       typename Second::second::pattern_type>
{
};

// last invocation step; evaluates a {projection, tpartial_function} pair
template<typename Data>
struct invoke_helper3
{
    Data const& data;
    invoke_helper3(Data const& mdata) : data(mdata) { }
    template<size_t Pos, typename T, typename... Args>
    inline bool operator()(util::type_pair<std::integral_constant<size_t, Pos>, T>,
                           Args&&... args) const
    {
        auto const& target = get<Pos>(data);
        return target.first(target.second, std::forward<Args>(args)...);
        //return (get<Pos>(data))(args...);
    }
};

template<class Data, class Token, class Pattern>
struct invoke_helper2
{
    typedef Pattern pattern_type;
    typedef typename util::tl_filter_not_type<pattern_type, anything>::type arg_types;
    Data const& data;
    invoke_helper2(Data const& mdata) : data(mdata) { }
    template<typename... Args>
    bool invoke(Args&&... args) const
    {
        typedef invoke_policy<Pattern> impl;
        return impl::invoke(*this, std::forward<Args>(args)...);
    }
    // resolved argument list (called from invoke_policy)
    template<typename... Args>
    bool operator()(Args&&... args) const
    {
        //static_assert(false, "foo");
        Token token;
        invoke_helper3<Data> fun{data};
        return util::static_foreach<0, Token::size>::eval_or(token, fun, std::forward<Args>(args)...);
    }
};

// invokes a group of {projection, tpartial_function} pairs
template<typename Data, typename BoolIter>
struct invoke_helper
{
    Data const& data;
    BoolIter enabled;
    invoke_helper(Data const& mdata, BoolIter biter) : data(mdata), enabled(biter) { }
    // token: type_list<type_pair<integral_constant<size_t, X>,
    //                            std::pair<projection, tpartial_function>>,
    //                  ...>
    // all {projection, tpartial_function} pairs have the same pattern
    // thus, can be invoked from same data
    template<class Token, typename... Args>
    bool operator()(Token, Args&&... args)
    {
        typedef typename Token::head type_pair;
        typedef typename type_pair::second leaf_pair;
        if (*enabled++)
        {
            // next invocation step
            invoke_helper2<Data,
                           Token,
                           typename leaf_pair::pattern_type> fun{data};
            return fun.invoke(std::forward<Args>(args)...);
        }
        //++enabled;
        return false;
    }
};

template<typename BoolArray>
struct can_invoke_helper
{
    BoolArray& data;
    size_t i;
    can_invoke_helper(BoolArray& mdata) : data(mdata), i(0) { }
    template<class Token, typename... Args>
    void operator()(Token, Args&&... args)
    {
        typedef typename Token::head type_pair;
        typedef typename type_pair::second leaf_pair;
        typedef invoke_policy<typename leaf_pair::pattern_type> impl;
        data[i++] = impl::can_invoke(std::forward<Args>(args)...);
    }
};

template<typename T>
struct is_manipulator_leaf
{
    static constexpr bool value = T::second_type::manipulates_args;
};

template<bool IsManipulator, typename T0, typename T1>
struct pj_fwd_
{
    typedef T1 type;
};

template<typename T>
struct pj_fwd_<false, T const&, T>
{
    typedef std::reference_wrapper<const T> type;
};

template<typename T>
struct pj_fwd_<true, T&, T>
{
    typedef std::reference_wrapper<T> type;
};

template<bool IsManipulator, typename T>
struct pj_fwd
{
    typedef typename pj_fwd_<
                IsManipulator,
                T,
                typename detail::implicit_conversions<
                    typename util::rm_ref<T>::type
                >::type
            >::type
            type;
};

/**
 * @brief A function that works on the projection of given data rather than
 *        on the data itself.
 */
template<class... Leaves>
class projected_fun
{

 public:

    typedef util::type_list<Leaves...> leaves_list;
    typedef typename util::tl_zip_with_index<leaves_list>::type zipped_list;
    typedef typename util::tl_group_by<zipped_list, pjf_same_pattern>::type
            eval_order;

    static constexpr bool has_manipulator =
            util::tl_exists<leaves_list, is_manipulator_leaf>::value;

    template<typename... Args>
    projected_fun(Args&&... args) : m_leaves(std::forward<Args>(args)...)
    {
        init();
    }

    projected_fun(projected_fun&& other) : m_leaves(std::move(other.m_leaves))
    {
        init();
    }

    projected_fun(projected_fun const& other) : m_leaves(other.m_leaves)
    {
        init();
    }

    bool invoke(any_tuple const& tup)
    {
        return _invoke(tup);
    }

    bool invoke(any_tuple& tup)
    {
        return _invoke(tup);
    }

    bool invoke(any_tuple&& tup)
    {
        any_tuple tmp{tup};
        return invoke(tmp);
    }

    template<typename... Args>
    bool can_invoke(Args&&... args)
    {
        typedef detail::tdata<typename pj_fwd<has_manipulator, Args>::type...>
                tuple_type;
        // applies implicit conversions etc
        tuple_type tup{std::forward<Args>(args)...};
        auto& type_token = typeid(typename tuple_type::types);
        eval_order token;
        cache_entry tmp;
        can_invoke_helper<cache_entry> fun{tmp};
        util::static_foreach<0, eval_order::size>
        ::_(token, fun, type_token, tup);
        return std::any_of(tmp.begin(), tmp.end(), [](bool value) { return value; });
    }

    template<typename... Args>
    bool operator()(Args&&... args)
    {
        typedef detail::tdata<typename pj_fwd<has_manipulator, Args>::type...>
                tuple_type;
        // applies implicit conversions etc
        tuple_type tup{std::forward<Args>(args)...};

        auto& type_token = typeid(typename tuple_type::types);
        auto enabled_begin = get_cache_entry(&type_token, tup);

        typedef typename util::if_else_c<
                    has_manipulator,
                    tuple_type&,
                    util::wrapped<tuple_type const&>
                >::type
                ref_type;

        typedef typename util::if_else_c<
                    has_manipulator,
                    void*,
                    util::wrapped<void const*>
                >::type
                ptr_type;

        eval_order token;
        invoke_helper<decltype(m_leaves), decltype(enabled_begin)> fun{m_leaves, enabled_begin};
        return util::static_foreach<0, eval_order::size>
                ::eval_or(token,
                          fun,
                          type_token,
                          detail::statically_typed,
                          static_cast<ptr_type>(nullptr),
                          static_cast<ref_type>(tup));
    }

    template<class... Rhs>
    projected_fun<Leaves..., Rhs...>
    or_else(projected_fun<Rhs...> const& other) const
    {
        detail::tdata<ge_reference_wrapper<Leaves>...,
                      ge_reference_wrapper<Rhs>...    > all_leaves;
        collect_tdata(all_leaves, m_leaves, other.leaves());
        return {all_leaves};
    }

    inline detail::tdata<Leaves...> const& leaves() const
    {
        return m_leaves;
    }

 private:

    // structure: tdata< tdata<type_list<...>, ...>,
    //                   tdata<type_list<...>, ...>,
    //                   ...>
    detail::tdata<Leaves...> m_leaves;

    static constexpr size_t cache_size = 10;
    typedef std::array<bool, eval_order::size> cache_entry;
    typedef typename cache_entry::iterator cache_entry_iterator;
    typedef std::pair<std::type_info const*, cache_entry> cache_element;


    util::fixed_vector<cache_element, cache_size> m_cache;

    // ring buffer like access to m_cache
    size_t m_cache_begin;
    size_t m_cache_end;

    cache_element m_dummy;

    static inline void advance_(size_t& i)
    {
        i = (i + 1) % cache_size;
    }

    inline size_t find_token_pos(std::type_info const* type_token)
    {
        for (size_t i = m_cache_begin ; i != m_cache_end; advance_(i))
        {
            if (m_cache[i].first == type_token) return i;
        }
        return m_cache_end;
    }

    template<class Tuple>
    cache_entry_iterator get_cache_entry(std::type_info const* type_token,
                                         Tuple const& value)
    {
        CPPA_REQUIRE(type_token != nullptr);
        if (value.impl_type() == detail::dynamically_typed)
        {
            return m_dummy.second.begin();
        }
        size_t i = find_token_pos(type_token);
        // if we didn't found a cache entry ...
        if (i == m_cache_end)
        {
            // ... 'create' one
            advance_(m_cache_end);
            if (m_cache_end == m_cache_begin) advance_(m_cache_begin);
            m_cache[i].first = type_token;
            eval_order token;
            can_invoke_helper<cache_entry> fun{m_cache[i].second};
            util::static_foreach<0, eval_order::size>
            ::_(token, fun, *type_token, value);
        }
        return m_cache[i].second.begin();
    }

    void init()
    {
        m_dummy.second.fill(true);
        m_cache.resize(cache_size);
        for(size_t i = 0; i < cache_size; ++i) m_cache[i].first = nullptr;
        m_cache_begin = m_cache_end = 0;
    }

    template<typename AbstractTuple, typename NativeDataPtr>
    bool _do_invoke(AbstractTuple& vals, NativeDataPtr ndp)
    {
        std::type_info const* type_token = vals.type_token();
        auto enabled_begin = get_cache_entry(type_token, vals);
        eval_order token;
        invoke_helper<decltype(m_leaves), decltype(enabled_begin)> fun{m_leaves, enabled_begin};
        return util::static_foreach<0, eval_order::size>
               ::eval_or(token,
                         fun,
                         *type_token,
                         vals.impl_type(),
                         ndp,
                         vals);
    }

    template<typename AnyTuple>
    bool _invoke(AnyTuple& tup,
                 typename util::enable_if_c<
                        std::is_const<AnyTuple>::value == false
                     && has_manipulator == true
                 >::type* = 0)
    {
        tup.force_detach();
        auto& vals = *(tup.vals());
        return _do_invoke(vals, vals.mutable_native_data());
    }

    template<typename AnyTuple>
    bool _invoke(AnyTuple& tup,
                 typename util::enable_if_c<
                        std::is_const<AnyTuple>::value == false
                     && has_manipulator == false
                 >::type* = 0)
    {
        return _invoke(static_cast<AnyTuple const&>(tup));
    }

    template<typename AnyTuple>
    bool _invoke(AnyTuple& tup,
                 typename util::enable_if_c<
                        std::is_const<AnyTuple>::value == true
                     && has_manipulator == false
                 >::type* = 0)
    {
        auto const& cvals = *(tup.cvals());
        return _do_invoke(cvals, cvals.native_data());
    }

    template<typename AnyTuple>
    bool _invoke(AnyTuple& tup,
                 typename util::enable_if_c<
                        std::is_const<AnyTuple>::value == true
                     && has_manipulator == true
                 >::type* = 0)
    {
        any_tuple tup_copy{tup};
        return _invoke(tup_copy);
    }

};

template<class List>
struct projected_fun_from_type_list;

template<typename... Args>
struct projected_fun_from_type_list<util::type_list<Args...> >
{
    typedef projected_fun<Args...> type;
};

template<typename... Lhs, typename... Rhs>
projected_fun<Lhs..., Rhs...> operator,(projected_fun<Lhs...> const& lhs,
                                        projected_fun<Rhs...> const& rhs)
{
    return lhs.or_else(rhs);
}

template<typename Arg0, typename... Args>
typename projected_fun_from_type_list<
    typename util::tl_concat<
        typename Arg0::leaves_list,
        typename Args::leaves_list...
    >::type
>::type
pj_concat(Arg0 const& arg0, Args const&... args)
{
    typename detail::tdata_from_type_list<
        typename util::tl_map<
            typename util::tl_concat<
                typename Arg0::leaves_list,
                typename Args::leaves_list...
            >::type,
            gref_wrapped
        >::type
    >::type
    all_leaves;
    collect_tdata(all_leaves, arg0.leaves(), args.leaves()...);
    return {all_leaves};
}

#define VERBOSE(LineOfCode) cout << #LineOfCode << " = " << (LineOfCode) << endl


struct cf_builder_from_args { };

template<class Guard, class Transformers, class Pattern>
struct cf_builder
{

    typedef typename detail::tdata_from_type_list<Transformers>::type
            fun_container;

    Guard m_guard;
    typename detail::tdata_from_type_list<Transformers>::type m_funs;

 public:

    cf_builder() = default;

    template<typename... Args>
    cf_builder(cf_builder_from_args const&, Args const&... args)
        : m_guard(args...)
        , m_funs(args...)
    {
    }

    cf_builder(Guard& mg, fun_container const& funs)
        : m_guard(std::move(mg)), m_funs(funs)
    {
    }

    cf_builder(Guard const& mg, fun_container const& funs)
        : m_guard(mg), m_funs(funs)
    {
    }

    template<typename NewGuard>
    cf_builder<
        guard_expr<
            logical_and_op,
            guard_expr<exec_xfun_op, Guard, util::void_type>,
            NewGuard>,
        Transformers,
        Pattern>
    when(NewGuard ng,
         typename util::disable_if_c<
               std::is_same<NewGuard, NewGuard>::value
            && std::is_same<Guard, value_guard< util::type_list<> >>::value
         >::type* = 0                                 ) const
    {
        return {(gcall(m_guard) && ng), m_funs};
    }

    template<typename NewGuard>
    cf_builder<NewGuard, Transformers, Pattern>
    when(NewGuard ng,
         typename util::enable_if_c<
               std::is_same<NewGuard, NewGuard>::value
            && std::is_same<Guard, value_guard< util::type_list<> >>::value
         >::type* = 0                                 ) const
    {
        return {ng, m_funs};
    }

    template<typename Expr>
    projected_fun<typename get_cfl<Expr, Guard, Transformers, Pattern>::type>
    operator>>(Expr expr) const
    {
        typedef typename get_cfl<Expr, Guard, Transformers, Pattern>::type tpair;
        return tpair{typename tpair::first_type{m_funs},
                     typename tpair::second_type{std::move(expr), m_guard}};
    }

};

template<bool IsFun, typename T>
struct add_ptr_to_fun_
{
    typedef T* type;
};

template<typename T>
struct add_ptr_to_fun_<false, T>
{
    typedef T type;
};

template<typename T>
struct add_ptr_to_fun : add_ptr_to_fun_<std::is_function<T>::value, T>
{
};

template<bool ToVoid, typename T>
struct to_void_impl
{
    typedef util::void_type type;
};

template<typename T>
struct to_void_impl<false, T>
{
    typedef typename add_ptr_to_fun<T>::type type;
};

template<typename T>
struct not_callable_to_void : to_void_impl<detail::is_boxed<T>::value || !util::is_callable<T>::value, T>
{
};

template<typename T>
struct boxed_and_callable_to_void : to_void_impl<detail::is_boxed<T>::value || util::is_callable<T>::value, T>
{
};

template<bool IsCallable, typename T>
struct pattern_type_
{
    typedef util::get_callable_trait<T> ctrait;
    typedef typename ctrait::arg_types args;
    static_assert(args::size == 1, "only unary functions allowed");
    typedef typename util::rm_ref<typename args::head>::type type;
};

template<typename T>
struct pattern_type_<false, T>
{
    typedef typename util::rm_ref<typename detail::unboxed<T>::type>::type type;
};

template<typename T>
struct pattern_type : pattern_type_<util::is_callable<T>::value && !detail::is_boxed<T>::value, T>
{
};

template<typename... T>
cf_builder<value_guard<util::type_list<> >,
           util::type_list<>,
           util::type_list<T...> >
_on()
{
    return {};
}

template<typename Arg0, typename... Args>
cf_builder<
    value_guard<
        typename util::tl_trim<
            typename util::tl_map<
                util::type_list<Arg0, Args...>,
                boxed_and_callable_to_void
            >::type
        >::type
    >,
    typename util::tl_map<
        util::type_list<Arg0, Args...>,
        not_callable_to_void
    >::type,
    util::type_list<typename pattern_type<Arg0>::type,
                    typename pattern_type<Args>::type...> >
_on(Arg0 const& arg0, Args const&... args)
{
    return {cf_builder_from_args{}, arg0, args...};
}

std::string int2str(int i)
{
    return std::to_string(i);
}

option<int> str2int(std::string const& str)
{
    char* endptr = nullptr;
    int result = static_cast<int>(strtol(str.c_str(), &endptr, 10));
    if (endptr != nullptr && *endptr == '\0')
    {
        return result;
    }
    return {};
}

typedef util::type_list<int, int, int, float, int, float, float> zz0;

typedef util::type_list<util::type_list<int, int, int>,
                        util::type_list<float>,
                        util::type_list<int>,
                        util::type_list<float, float> > zz8;

typedef util::type_list<
            util::type_list<
                util::type_pair<std::integral_constant<size_t,0>, int>,
                util::type_pair<std::integral_constant<size_t,1>, int>,
                util::type_pair<std::integral_constant<size_t,2>, int>
            >,
            util::type_list<
                util::type_pair<std::integral_constant<size_t,3>, float>
            >,
            util::type_list<
                util::type_pair<std::integral_constant<size_t,4>, int>
            >,
            util::type_list<
                util::type_pair<std::integral_constant<size_t,5>, float>,
                util::type_pair<std::integral_constant<size_t,6>, float>
            >
        >
        zz9;


template<typename First, typename Second>
struct is_same_ : std::is_same<typename First::second, typename Second::second>
{
};

#define CPPA_CHECK_INVOKED(FunName, Args)                                      \
    if ( ( FunName Args ) == false || invoked != #FunName ) {                  \
        CPPA_ERROR("invocation of " #FunName " failed");                       \
    } invoked = ""

#define CPPA_CHECK_NOT_INVOKED(FunName, Args)                                  \
    if ( ( FunName Args ) == true || invoked == #FunName ) {                   \
        CPPA_ERROR(#FunName " erroneously invoked");                           \
    } invoked = ""

size_t test__tuple()
{
    CPPA_TEST(test__tuple);

    using namespace cppa::placeholders;

    typedef typename util::tl_group_by<zz0, std::is_same>::type zz1;

    typedef typename util::tl_zip_with_index<zz0>::type zz2;

    static_assert(std::is_same<zz1, zz8>::value, "group_by failed");

    typedef typename util::tl_group_by<zz2, is_same_>::type zz3;

    static_assert(std::is_same<zz3, zz9>::value, "group_by failed");

    typedef util::type_list<int, int> token1;
    typedef util::type_list<float> token2;

    std::string invoked;

    auto f00 = _on<int, int>() >> [&]() { invoked = "f00"; };
    CPPA_CHECK_INVOKED(f00, (42, 42));

    auto f01 = _on<int, int>().when(_x1 == 42) >> [&]() { invoked = "f01"; };
    CPPA_CHECK_INVOKED(f01, (42, 42));
    CPPA_CHECK_NOT_INVOKED(f01, (1, 2));

    auto f02 = _on<int, int>().when(_x1 == 42 && _x2 * 2 == _x1) >> [&]() { invoked = "f02"; };
    CPPA_CHECK_NOT_INVOKED(f02, (0, 0));
    CPPA_CHECK_NOT_INVOKED(f02, (42, 42));
    CPPA_CHECK_NOT_INVOKED(f02, (2, 1));
    CPPA_CHECK_INVOKED(f02, (42, 21));

    CPPA_CHECK(f02.invoke(make_cow_tuple(42, 21)));
    CPPA_CHECK_EQUAL("f02", invoked);
    invoked = "";

    auto f03 = _on(42, val<int>) >> [&](int const& a, int&) { invoked = "f03"; CPPA_CHECK_EQUAL(42, a); };
    CPPA_CHECK_NOT_INVOKED(f03, (0, 0));
    CPPA_CHECK_INVOKED(f03, (42, 42));

    auto f04 = _on(42, int2str).when(_x2 == "42") >> [&](std::string& str)
    {
        CPPA_CHECK_EQUAL("42", str);
        invoked = "f04";
    };

    CPPA_CHECK_NOT_INVOKED(f04, (0, 0));
    CPPA_CHECK_NOT_INVOKED(f04, (0, 42));
    CPPA_CHECK_NOT_INVOKED(f04, (42, 0));
    CPPA_CHECK_INVOKED(f04, (42, 42));

    auto f05 = _on(str2int).when(_x1 % 2 == 0) >> [&]() { invoked = "f05"; };
    CPPA_CHECK_NOT_INVOKED(f05, ("1"));
    CPPA_CHECK_INVOKED(f05, ("2"));

    auto f06 = _on(42, str2int).when(_x2 % 2 == 0) >> [&]() { invoked = "f06"; };
    CPPA_CHECK_NOT_INVOKED(f06, (0, "0"));
    CPPA_CHECK_NOT_INVOKED(f06, (42, "1"));
    CPPA_CHECK_INVOKED(f06, (42, "2"));

    int f07_val = 1;
    auto f07 = _on<int>().when(_x1 == gref(f07_val)) >> [&]() { invoked = "f07"; };
    CPPA_CHECK_NOT_INVOKED(f07, (0));
    CPPA_CHECK_INVOKED(f07, (1));
    CPPA_CHECK_NOT_INVOKED(f07, (2));
    ++f07_val;
    CPPA_CHECK_NOT_INVOKED(f07, (0));
    CPPA_CHECK_NOT_INVOKED(f07, (1));
    CPPA_CHECK_INVOKED(f07, (2));
    CPPA_CHECK(f07.invoke(make_cow_tuple(2)));

    int f08_val = 666;
    auto f08 = _on<int>() >> [&](int& mref) { mref = 8; invoked = "f08"; };
    CPPA_CHECK_INVOKED(f08, (f08_val));
    CPPA_CHECK_EQUAL(8, f08_val);
    any_tuple f08_any_val = make_cow_tuple(666);
    CPPA_CHECK(f08.invoke(f08_any_val));
    CPPA_CHECK_EQUAL(8, f08_any_val.get_as<int>(0));

    int f09_val = 666;
    auto f09 = _on(str2int, val<int>) >> [&](int& mref) { mref = 9; invoked = "f09"; };
    CPPA_CHECK_NOT_INVOKED(f09, ("hello lambda", f09_val));
    CPPA_CHECK_INVOKED(f09, ("0", f09_val));
    CPPA_CHECK_EQUAL(9, f09_val);
    any_tuple f09_any_val = make_cow_tuple("0", 666);
    CPPA_CHECK(f09.invoke(f09_any_val));
    CPPA_CHECK_EQUAL(9, f09_any_val.get_as<int>(1));
    f09_any_val.get_as_mutable<int>(1) = 666;
    any_tuple f09_any_val_copy{f09_any_val};
    CPPA_CHECK_EQUAL(f09_any_val.at(0), f09_any_val_copy.at(0));
    // detaches f09_any_val from f09_any_val_copy
    CPPA_CHECK(f09.invoke(f09_any_val));
    CPPA_CHECK_EQUAL(9, f09_any_val.get_as<int>(1));
    CPPA_CHECK_EQUAL(666, f09_any_val_copy.get_as<int>(1));
    // no longer the same data
    CPPA_CHECK_NOT_EQUAL(f09_any_val.at(0), f09_any_val_copy.at(0));

    auto f10 =
    (
        _on<int>().when(_x1 < 10)    >> [&]() { invoked = "f10.0"; },
        _on<int>()                   >> [&]() { invoked = "f10.1"; },
        _on<std::string, anything>() >> [&](std::string&) { invoked = "f10.2"; }
    );

    CPPA_CHECK(f10(9));
    CPPA_CHECK_EQUAL("f10.0", invoked);
    CPPA_CHECK(f10(10));
    CPPA_CHECK_EQUAL("f10.1", invoked);
    CPPA_CHECK(f10("42"));
    CPPA_CHECK_EQUAL("f10.2", invoked);
    CPPA_CHECK(f10("42", 42));
    CPPA_CHECK(f10("a", "b", "c"));
    std::string foobar = "foobar";
    CPPA_CHECK(f10(foobar, "b", "c"));
    CPPA_CHECK(f10("a", static_cast<std::string const&>(foobar), "b", "c"));
    //CPPA_CHECK(f10(static_cast<std::string const&>(foobar), "b", "c"));

    int f11_fun = 0;
    auto f11 = pj_concat
    (
        _on<int>().when(_x1 == 1) >> [&]() { f11_fun =  1; },
        _on<int>().when(_x1 == 2) >> [&]() { f11_fun =  2; },
        _on<int>().when(_x1 == 3) >> [&]() { f11_fun =  3; },
        _on<int>().when(_x1 == 4) >> [&]() { f11_fun =  4; },
        _on<int>().when(_x1 == 5) >> [&]() { f11_fun =  5; },
        _on<int>().when(_x1 == 6) >> [&]() { f11_fun =  6; },
        _on<int>().when(_x1 == 7) >> [&]() { f11_fun =  7; },
        _on<int>().when(_x1 == 8) >> [&]() { f11_fun =  8; },
        _on<int>().when(_x1 >= 9) >> [&]() { f11_fun =  9; },
        _on(str2int)              >> [&]() { f11_fun = 10; },
        _on<std::string>()        >> [&]() { f11_fun = 11; }
    );

    CPPA_CHECK(f11(1));
    CPPA_CHECK_EQUAL(1, f11_fun);
    CPPA_CHECK(f11(3));
    CPPA_CHECK_EQUAL(3, f11_fun);
    CPPA_CHECK(f11(8));
    CPPA_CHECK_EQUAL(8, f11_fun);
    CPPA_CHECK(f11(10));
    CPPA_CHECK_EQUAL(9, f11_fun);
    CPPA_CHECK(f11("hello lambda"));
    CPPA_CHECK_EQUAL(11, f11_fun);
    CPPA_CHECK(f11("10"));
    CPPA_CHECK_EQUAL(10, f11_fun);

    auto f12 = pj_concat
    (
        _on<int, anything, int>().when(_x1 < _x2) >> [&](int a, int b)
        {
            CPPA_CHECK_EQUAL(1, a);
            CPPA_CHECK_EQUAL(5, b);
            invoked = "f12";
        }
    );
    CPPA_CHECK_INVOKED(f12, (1, 2, 3, 4, 5));

    int f13_fun = 0;
    auto f13 = pj_concat
    (
        _on<int, anything, std::string, anything, int>().when(_x1 < _x3 && _x2.starts_with("-")) >> [&](int a, std::string const& str, int b)
        {
            CPPA_CHECK_EQUAL("-h", str);
            CPPA_CHECK_EQUAL(1, a);
            CPPA_CHECK_EQUAL(10, b);
            f13_fun = 1;
            invoked = "f13";
        },
        _on<anything, std::string, anything, int, anything, float, anything>() >> [&](std::string const& str, int a, float b)
        {
            CPPA_CHECK_EQUAL("h", str);
            CPPA_CHECK_EQUAL(12, a);
            CPPA_CHECK_EQUAL(1.f, b);
            f13_fun = 2;
            invoked = "f13";
        },
        _on<float, anything, float>().when(_x1 * 2 == _x2) >> [&](float a, float b)
        {
            CPPA_CHECK_EQUAL(1.f, a);
            CPPA_CHECK_EQUAL(2.f, b);
            f13_fun = 3;
            invoked = "f13";
        }
    );
    CPPA_CHECK_INVOKED(f13, (1, 2, "-h", 12, 32, 10, 1.f, "--foo", 10));
    CPPA_CHECK_EQUAL(1, f13_fun);
    CPPA_CHECK_INVOKED(f13, (1, 2, "h", 12, 32, 10, 1.f, "--foo", 10));
    CPPA_CHECK_EQUAL(2, f13_fun);
    CPPA_CHECK_INVOKED(f13, (1.f, 1.5f, 2.f));
    CPPA_CHECK_EQUAL(3, f13_fun);

    //exit(0);

    return CPPA_TEST_RESULT;

    auto old_pf =
    (
        on(42) >> []() { },
        on("abc") >> []() { },
        on<int, int>() >> []() { },
        on<anything>() >> []() { }
    );

    auto new_pf =
    (
        _on(42) >> []() { },
        _on(std::string("abc")) >> []() { },
        _on<int, int>() >> []() { },
        _on<anything>() >> []() { }
    );

    any_tuple testee[] = {
        make_cow_tuple(42),
        make_cow_tuple("abc"),
        make_cow_tuple("42"),
        make_cow_tuple(1, 2),
        make_cow_tuple(1, 2, 3)
    };

    constexpr size_t numInvokes = 100000000;

    /*
    auto xvals = make_cow_tuple(1, 2, "3");

    std::string three{"3"};

    std::atomic<std::string*> p3{&three};

    auto guard1 = _x1 == 1;
    auto guard2 = _x1 + _x2 == 3;
    auto guard3 = _x1 + _x2 == 3 && _x3 == "3";

    int dummy_counter = 0;

    cout << "time for for " << numInvokes << " guards(1)*" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < numInvokes; ++i)
            if (guard1(1, 2, *p3))//const_cast<std::string&>(three)))
                ++dummy_counter;
    }

    cout << "time for for " << numInvokes << " guards(1)*" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < numInvokes; ++i)
            if (guard1(get_ref<0>(xvals), get_ref<1>(xvals), get_ref<2>(xvals)))
                ++dummy_counter;
    }

    cout << "time for for " << numInvokes << " guards(1)" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < numInvokes; ++i)
            if (util::unchecked_apply_tuple<bool>(guard1, xvals))
                ++dummy_counter;
    }

    cout << "time for for " << numInvokes << " guards(2)" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < numInvokes; ++i)
            if (util::unchecked_apply_tuple<bool>(guard2, xvals))
                ++dummy_counter;
    }

    cout << "time for for " << numInvokes << " guards(3)" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < numInvokes; ++i)
            if (util::unchecked_apply_tuple<bool>(guard3, xvals))
                ++dummy_counter;
    }

    cout << "time for " << numInvokes << " equal if-statements" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < (numInvokes / sizeof(testee)); ++i)
        {
            if (get<0>(xvals) + get<1>(xvals) == 3 && get<2>(xvals) == "3")
            {
                ++dummy_counter;
            }
        }
    }
    */

    cout << "old partial function implementation for " << numInvokes << " matches" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < (numInvokes / sizeof(testee)); ++i)
        {
            for (auto& x : testee) { old_pf(x); }
        }
    }

    cout << "new partial function implementation for " << numInvokes << " matches" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < (numInvokes / sizeof(testee)); ++i)
        {
            for (auto& x : testee) { new_pf.invoke(x); }
        }
    }

    cout << "old partial function with on() inside loop" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < (numInvokes / sizeof(testee)); ++i)
        {
            auto tmp =
            (
                on(42) >> []() { },
                on("abc") >> []() { },
                on<int, int>() >> []() { },
                on<anything>() >> []() { }
            );
            for (auto& x : testee) { tmp(x); }
        }
    }

    cout << "new partial function with on() inside loop" << endl;
    {
        boost::progress_timer t0;
        for (size_t i = 0; i < (numInvokes / sizeof(testee)); ++i)
        {
            auto tmp =
            (
                _on(42) >> []() { },
                _on(std::string("abc")) >> []() { },
                _on<int, int>() >> []() { },
                _on<anything>() >> []() { }
            );
            for (auto& x : testee) { tmp(x); }
        }
    }

    exit(0);

    /*
    VERBOSE(f00(42, 42));
    VERBOSE(f01(42, 42));
    VERBOSE(f02(42, 42));
    VERBOSE(f02(42, 21));
    VERBOSE(f03(42, 42));

    cout << detail::demangle(typeid(f04).name()) << endl;

    VERBOSE(f04(42, 42));
    VERBOSE(f04(42, std::string("42")));

    VERBOSE(f05(42, 42));
    VERBOSE(f05(42, std::string("41")));
    VERBOSE(f05(42, std::string("42")));
    VERBOSE(f05(42, std::string("hello world!")));

    auto f06 = f04.or_else(_on<int, int>().when(_x2 > _x1) >> []() { });
    VERBOSE(f06(42, 42));
    VERBOSE(f06(1, 2));
    */

    /*
    auto f06 = _on<anything, int>() >> []() { };

    VERBOSE(f06(1));
    VERBOSE(f06(1.f, 2));
*/

    /*

    auto f0 = cfun<token1>([](int, int) { cout << "f0[0]!" << endl; }, _x1 < _x2)
             //.or_else(f00)
             .or_else(cfun<token1>([](int, int) { cout << "f0[1]!" << endl; }, _x1 > _x2))
             .or_else(cfun<token1>([](int, int) { cout << "f0[2]!" << endl; }, _x1 == 2 && _x2 == 2))
             .or_else(cfun<token2>([](float) { cout << "f0[3]!" << endl; }, value_guard< util::type_list<> >{}))
             .or_else(cfun<token1>([](int, int) { cout << "f0[4]" << endl; }, value_guard< util::type_list<> >{}));

    //VERBOSE(f0(make_cow_tuple(1, 2)));

    VERBOSE(f0(3, 3));
    VERBOSE(f0.invoke(make_cow_tuple(3, 3)));

    VERBOSE(f0.invoke(make_cow_tuple(2, 2)));
    VERBOSE(f0.invoke(make_cow_tuple(3, 2)));
    VERBOSE(f0.invoke(make_cow_tuple(1.f)));

    auto f1 = cfun<token1>([](float, int) { cout << "f1!" << endl; }, _x1 < 6, tofloat);

    VERBOSE(f1.invoke(make_cow_tuple(5, 6)));
    VERBOSE(f1.invoke(make_cow_tuple(6, 7)));

    auto i2 = make_cow_tuple(1, 2);

    VERBOSE(f0.invoke(*i2.vals()->type_token(), i2.vals()->impl_type(), i2.vals()->native_data(), *(i2.vals())));
    VERBOSE(f1.invoke(*i2.vals()->type_token(), i2.vals()->impl_type(), i2.vals()->native_data(), *(i2.vals())));

    any_tuple dt1;
    {
        auto oarr = new detail::object_array;
        oarr->push_back(object::from(1));
        oarr->push_back(object::from(2));
        dt1 = any_tuple{static_cast<detail::abstract_tuple*>(oarr)};
    }


    VERBOSE(f0.invoke(*dt1.cvals()->type_token(), dt1.cvals()->impl_type(), dt1.cvals()->native_data(), *dt1.cvals()));
    VERBOSE(f1.invoke(*dt1.cvals()->type_token(), dt1.cvals()->impl_type(), dt1.cvals()->native_data(), *dt1.cvals()));

    */

    // check type correctness of make_cow_tuple()
    auto t0 = make_cow_tuple("1", 2);
    CPPA_CHECK((std::is_same<decltype(t0), cppa::cow_tuple<std::string, int>>::value));
    auto t0_0 = get<0>(t0);
    auto t0_1 = get<1>(t0);
    // check implicit type conversion
    CPPA_CHECK((std::is_same<decltype(t0_0), std::string>::value));
    CPPA_CHECK((std::is_same<decltype(t0_1), int>::value));
    CPPA_CHECK_EQUAL(t0_0, "1");
    CPPA_CHECK_EQUAL(t0_1, 2);
    // use tuple cast to get a subtuple
    any_tuple at0(t0);
    auto v0opt = tuple_cast<std::string, anything>(at0);
    CPPA_CHECK((std::is_same<decltype(v0opt), option<cow_tuple<std::string>>>::value));
    CPPA_CHECK((v0opt));
    CPPA_CHECK(   at0.size() == 2
               && at0.at(0) == &get<0>(t0)
               && at0.at(1) == &get<1>(t0));
    if (v0opt)
    {
        auto& v0 = *v0opt;
        CPPA_CHECK((std::is_same<decltype(v0), cow_tuple<std::string>&>::value));
        CPPA_CHECK((std::is_same<decltype(get<0>(v0)), std::string const&>::value));
        CPPA_CHECK_EQUAL(v0.size(), 1);
        CPPA_CHECK_EQUAL(get<0>(v0), "1");
        CPPA_CHECK_EQUAL(get<0>(t0), get<0>(v0));
        // check cow semantics
        CPPA_CHECK_EQUAL(&get<0>(t0), &get<0>(v0));     // point to the same
        get_ref<0>(t0) = "hello world";                 // detaches t0 from v0
        CPPA_CHECK_EQUAL(get<0>(t0), "hello world");    // t0 contains new value
        CPPA_CHECK_EQUAL(get<0>(v0), "1");              // v0 contains old value
        CPPA_CHECK_NOT_EQUAL(&get<0>(t0), &get<0>(v0)); // no longer the same
        // check operator==
        auto lhs = make_cow_tuple(1,2,3,4);
        auto rhs = make_cow_tuple(static_cast<std::uint8_t>(1), 2.0, 3, 4);
        CPPA_CHECK(lhs == rhs);
        CPPA_CHECK(rhs == lhs);
    }
    any_tuple at1 = make_cow_tuple("one", 2, 3.f, 4.0);
    {
        // perfect match
        auto opt0 = tuple_cast<std::string, int, float, double>(at1);
        CPPA_CHECK(opt0);
        if (opt0)
        {
            CPPA_CHECK((*opt0 == make_cow_tuple("one", 2, 3.f, 4.0)));
            CPPA_CHECK_EQUAL(&get<0>(*opt0), at1.at(0));
            CPPA_CHECK_EQUAL(&get<1>(*opt0), at1.at(1));
            CPPA_CHECK_EQUAL(&get<2>(*opt0), at1.at(2));
            CPPA_CHECK_EQUAL(&get<3>(*opt0), at1.at(3));
        }
        // leading wildcard
        auto opt1 = tuple_cast<anything, double>(at1);
        CPPA_CHECK(opt1);
        if (opt1)
        {
            CPPA_CHECK_EQUAL(get<0>(*opt1), 4.0);
            CPPA_CHECK_EQUAL(&get<0>(*opt1), at1.at(3));
        }
        // trailing wildcard
        auto opt2 = tuple_cast<std::string, anything>(at1);
        CPPA_CHECK(opt2);
        if (opt2)
        {
            CPPA_CHECK_EQUAL(get<0>(*opt2), "one");
            CPPA_CHECK_EQUAL(&get<0>(*opt2), at1.at(0));
        }
        // wildcard in between
        auto opt3 = tuple_cast<std::string, anything, double>(at1);
        CPPA_CHECK(opt3);
        if (opt3)
        {
            CPPA_CHECK((*opt3 == make_cow_tuple("one", 4.0)));
            CPPA_CHECK_EQUAL(get<0>(*opt3), "one");
            CPPA_CHECK_EQUAL(get<1>(*opt3), 4.0);
            CPPA_CHECK_EQUAL(&get<0>(*opt3), at1.at(0));
            CPPA_CHECK_EQUAL(&get<1>(*opt3), at1.at(3));
        }
    }
    return CPPA_TEST_RESULT;
}
