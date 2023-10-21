#pragma once

#include <dingo/config.h>

#include <dingo/allocator.h>
#include <dingo/annotated.h>
#include <dingo/class_instance_factory.h>
#include <dingo/class_instance_factory_traits.h>
#include <dingo/collection_traits.h>
#include <dingo/decay.h>
#include <dingo/exceptions.h>
#include <dingo/resolving_context.h>
#include <dingo/rtti.h>
#include <dingo/storage/shared_cyclical.h>
#include <dingo/storage/unique.h>
#include <dingo/type_map.h>
#include <dingo/type_registration.h>

#include <functional>
#include <map>
#include <optional>
#include <typeindex>

namespace dingo {

template <typename T, typename Tag> class static_allocator {
  public:
    using value_type = T;

    static_allocator() noexcept {}
    template <typename U>
    static_allocator(const static_allocator<U, Tag>&) noexcept {}

    value_type* allocate(std::size_t n) {
        (void)n;
        assert(n == 1);
        if (used_)
            return nullptr;
        used_ = true;
        return reinterpret_cast<value_type*>(&storage_);
    }

    void deallocate(value_type* p, std::size_t n) noexcept {
        (void)p;
        (void)n;
        assert(used_);
        assert(n == 1);
        assert(p == reinterpret_cast<value_type*>(&storage_));
        used_ = false;
    }

  private:
    static std::aligned_storage_t<sizeof(T), alignof(T)> storage_;
    static bool used_;
};

template <typename T, typename Tag> bool static_allocator<T, Tag>::used_;
template <typename T, typename Tag>
std::aligned_storage_t<sizeof(T), alignof(T)>
    static_allocator<T, Tag>::storage_;

struct dynamic_container_traits {
    template <typename> using rebind_t = dynamic_container_traits;

    using tag_type = void;
    using rtti_type = dynamic_rtti;
    template <typename Value, typename Allocator>
    using type_factory_map_type = dynamic_type_map<rtti_type, Value, Allocator>;
    using allocator_type = std::allocator<char>;
};

template <typename Tag = void> struct static_container_traits {
    template <typename TagT> using rebind_t = static_container_traits<TagT>;

    using tag_type = Tag;
    using rtti_type = static_rtti;
    template <typename Value, typename Allocator>
    using type_factory_map_type =
        static_type_map<rtti_type, Tag, Value, Allocator>;
    using allocator_type = static_allocator<char, Tag>;
};

template <typename Traits>
static constexpr bool is_tagged_container_v =
    !std::is_same_v<typename Traits::template rebind_t<
                        type_list<typename Traits::tag_type, void>>,
                    Traits>;

// TODO wrt. multibindinds, is this like map/multimap?
template <typename ContainerTraits = dynamic_container_traits,
          typename Allocator = typename ContainerTraits::allocator_type,
          typename ParentContainer = void>
class container : public allocator_base<Allocator> {
    friend class resolving_context;

    template <typename ContainerTraitsT, typename AllocatorT,
              typename ParentContainerT>
    using rebind_t = container<ContainerTraitsT, AllocatorT, ParentContainerT>;
    using container_type =
        container<ContainerTraits, Allocator, ParentContainer>;
    using parent_container_type =
        std::conditional_t<std::is_same_v<void, ParentContainer>,
                           container_type, ParentContainer>;

  public:
    using container_traits_type = ContainerTraits;
    using allocator_type = Allocator;
    using rtti_type = typename ContainerTraits::rtti_type;

    template <typename Tag>
    using child_container_type =
        container<typename container_traits_type::template rebind_t<
                      type_list<typename container_traits_type::tag_type, Tag>>,
                  Allocator, container_type>;

    container()
        : allocator_base<allocator_type>(allocator_type()),
          type_factories_(get_allocator()) {}

    container(allocator_type alloc)
        : allocator_base<allocator_type>(alloc),
          type_factories_(get_allocator()) {}

    container(parent_container_type* parent,
              allocator_type alloc = allocator_type())
        : allocator_base<allocator_type>(alloc), parent_(parent),
          type_factories_(get_allocator()) {

        static_assert(
            !is_tagged_container_v<container_traits_type> ||
                !std::is_same_v<typename container_traits_type::tag_type,
                                typename parent_container_type::
                                    container_traits_type::tag_type>,
            "static typemap based containers require parent and child "
            "container tags to be different");
    }

    template <typename... TypeArgs> auto& register_type() {
        using registration = type_registration<TypeArgs...>;
        using storage_type =
            detail::storage<typename registration::scope_type::type,
                            typename registration::storage_type::type,
                            typename registration::factory_type::type,
                            typename registration::conversions_type::type>;

        using class_instance_container_type =
            typename container_type::template rebind_t<
                typename ContainerTraits::template rebind_t<
                    type_list<typename ContainerTraits::tag_type,
                              typename registration::interface_type>>,
                allocator_type, container_type>;

        using class_instance_data_type =
            class_instance_data<class_instance_container_type, storage_type>;

        // TODO: remove tuple usage... or remove type_list usage
        if constexpr (std::tuple_size_v<
                          typename registration::interface_type::type_tuple> ==
                      1) {
            using interface_type = std::tuple_element_t<
                0, typename registration::interface_type::type_tuple>;

            using class_instance_factory_type = class_instance_factory<
                container_type, typename annotated_traits<interface_type>::type,
                storage_type, class_instance_data_type>;

            auto&& [factory, factory_container] =
                allocate_factory<class_instance_factory_type>(this);
            register_type_factory<interface_type, storage_type>(
                std::move(factory));
            return *factory_container;
        } else {
            auto data = std::allocate_shared<class_instance_data_type>(
                allocator_traits::rebind<class_instance_data_type>(
                    get_allocator()),
                this);
            // TODO: this should deregister all interfaces that registered
            for_each(
                typename registration::interface_type::type{},
                [&](auto element) {
                    using interface_type = typename decltype(element)::type;

                    using class_instance_factory_type = class_instance_factory<
                        container_type,
                        typename annotated_traits<interface_type>::type,
                        storage_type,
                        std::shared_ptr<class_instance_data_type>>;

                    register_type_factory<interface_type, storage_type>(
                        allocate_factory<class_instance_factory_type>(data)
                            .first);
                });
            return data->container;
        }
    }

    template <typename... TypeArgs, typename Arg>
    auto& register_type(Arg&& arg) {
        using registration = type_registration<TypeArgs..., factory<Arg>>;

        using storage_type =
            detail::storage<typename registration::scope_type::type,
                            typename registration::storage_type::type,
                            typename registration::factory_type::type,
                            typename registration::conversions_type::type>;

        using class_instance_container_type =
            typename container_type::template rebind_t<
                typename ContainerTraits::template rebind_t<
                    type_list<typename ContainerTraits::tag_type,
                              typename registration::interface_type>>,
                allocator_type, container_type>;

        using class_instance_data_type =
            class_instance_data<class_instance_container_type, storage_type>;

        if constexpr (std::tuple_size_v<
                          typename registration::interface_type::type_tuple> ==
                      1) {
            using interface_type = std::tuple_element_t<
                0, typename registration::interface_type::type_tuple>;

            using class_instance_factory_type = class_instance_factory<
                container_type, typename annotated_traits<interface_type>::type,
                storage_type, class_instance_data_type>;

            auto&& [factory, factory_container] =
                allocate_factory<class_instance_factory_type>(
                    this, std::forward<Arg>(arg));
            register_type_factory<interface_type, storage_type>(
                std::move(factory));
            return *factory_container;
        } else {
            auto data = std::allocate_shared<class_instance_data_type>(
                allocator_traits::rebind<class_instance_data_type>(
                    get_allocator()),
                this, std::forward<Arg>(arg));

            for_each(
                typename registration::interface_type::type{},
                [&](auto element) {
                    using interface_type = typename decltype(element)::type;

                    using class_instance_factory_type = class_instance_factory<
                        container_type,
                        typename annotated_traits<interface_type>::type,
                        storage_type,
                        std::shared_ptr<class_instance_data_type>>;

                    register_type_factory<interface_type, storage_type>(
                        allocate_factory<class_instance_factory_type>(data)
                            .first);
                });
            return data->container;
        }
    }

    template <typename T,
              typename R = typename annotated_traits<
                  std::conditional_t<std::is_rvalue_reference_v<T>,
                                     std::remove_reference_t<T>, T>>::type>
    R resolve() {
        // TODO: it is the destructor that is slowing the resolving
        // std::aligned_storage_t< sizeof(resolving_context< container_type >) >
        // storage; new (&storage) resolving_context< container_type >(*this);
        // auto& context = *reinterpret_cast< resolving_context< container_type
        // >*
        // >(&storage);

        // TODO: context needs to be placed here, but initialized on the fly
        // later if needed. The only use of it is for cleanup during exception
        // and for cyclical types.

        // TODO: another idea - returning address of an temporary is fast
        // So what if we create home for that temporary and in-place construct
        // the unique object there? This is now in resolver_.

        resolving_context context;
        return resolve<T, true>(context);
    }

    template <typename T, typename Factory = constructor<decay_t<T>>>
    T construct(Factory factory = Factory()) {
        // TODO: nothrow constructuble
        resolving_context context;
        return factory.template construct<T>(context, *this);
    }

    // private:
    template <typename TypeInterface, typename TypeStorage, typename Factory>
    void register_type_factory(Factory&& factory) {
        // static_allocator returns null in the case an allocation (eg. the
        // factory can be null) There is no need to throw here as the insertion
        // will not be done later.

        check_interface_requirements<
            TypeStorage, typename annotated_traits<TypeInterface>::type,
            typename TypeStorage::type>();

        auto pb = type_factories_.template insert<TypeInterface>(
            typename ContainerTraits::template type_factory_map_type<
                class_instance_factory_ptr<
                    class_instance_factory_i<container_type>>,
                allocator_type>(get_allocator()));
        if (!pb.first
                 .template insert<
                     type_list<TypeInterface, typename TypeStorage::type>>(
                     std::forward<Factory>(factory))
                 .second) {
            throw type_already_registered_exception();
        }
    }

    template <typename T, bool RemoveRvalueReferences,
              typename R = std::conditional_t<
                  RemoveRvalueReferences,
                  std::conditional_t<std::is_rvalue_reference_v<T>,
                                     std::remove_reference_t<T>, T>,
                  T>>
    R resolve(resolving_context& context) {
        using Type = decay_t<T>;

        auto factories = type_factories_.template get<Type>();
        if (factories) {
            if (factories->size() == 1) {
                auto& factory = factories->front();
                return class_instance_factory_traits<
                    rtti_type,
                    typename annotated_traits<T>::type>::resolve(*factory,
                                                                 context);
            } else {
                return resolve_multiple<T>(context);
            }
        } else if constexpr (!std::is_same_v<void*, decltype(parent_)>) {
            if (parent_)
                return parent_->template resolve<T, RemoveRvalueReferences>(
                    context);
        }

        throw type_not_found_exception();
    }

    template <typename T>
    std::enable_if_t<!collection_traits<decay_t<T>>::is_collection, T>
    resolve_multiple(resolving_context&) {
        throw type_not_found_exception();
    }

#if 0
    // TODO
    template <typename T>
    std::enable_if_t<collection_traits<decay_t<T>>::is_collection, T>
    resolve_multiple(resolving_context& context) {
        using Type = decay_t<T>;
        using ValueType = decay_t<typename Type::value_type>;

        auto range = type_factories_.equal_range(typeid(ValueType));

        // TODO: destructor for results is not called, leaking the memory.
        // Unfortunatelly for the case when this is called from resolve(),
        // return value is T&.
        auto results = context.template get_allocator<Type>().allocate(1);
        new (results) Type;
        collection_traits<Type>::reserve(*results, std::distance(range.first, range.second));
        if (range.first != range.second) {
            for (auto it = range.first; it != range.second; ++it) {
                collection_traits<Type>::add(
                    *results,
                    class_instance_factory_traits<rtti_type, typename Type::value_type>::resolve(*it->second, context));
            }

            return *results;
        }

        throw type_not_found_exception();
    }
#endif

    template <class Storage, class TypeInterface, class Type>
    void check_interface_requirements() {
        static_assert(!std::is_reference_v<TypeInterface>);
        static_assert(
            std::is_convertible_v<decay_t<Type>*, decay_t<TypeInterface>*>);
        if constexpr (!std::is_same_v<decay_t<Type>, decay_t<TypeInterface>>) {
            static_assert(detail::storage_interface_requirements_v<
                              Storage, decay_t<Type>, decay_t<TypeInterface>>,
                          "storage requirements not met");
        }
    }

    template <typename U, typename... Args>
    std::pair<
        class_instance_factory_ptr<class_instance_factory_i<container_type>>,
        typename U::data_traits::container_type*>
    allocate_factory(Args&&... args) {
        auto alloc = allocator_traits::rebind<U>(get_allocator());
        U* instance = allocator_traits::allocate(alloc, 1);
        if (!instance)
            return {nullptr, nullptr};

        // TODO: should be nothrow-constructible
        // static_assert(std::is_nothrow_constructible_v<U, Args...>);
        allocator_traits::construct(alloc, instance,
                                    std::forward<Args>(args)...);

        return {instance, &instance->get_container()};
    }

    allocator_type& get_allocator() {
        return allocator_base<allocator_type>::get_allocator();
    }

    parent_container_type* parent_ = nullptr;

    typename ContainerTraits::template type_factory_map_type<
        typename ContainerTraits::template type_factory_map_type<
            class_instance_factory_ptr<
                class_instance_factory_i<container_type>>,
            allocator_type>,
        allocator_type>
        type_factories_;
};
} // namespace dingo