#include <dingo/container.h>
#include <dingo/storage/shared.h>
#include <dingo/storage/shared_cyclical.h>
#include <dingo/storage/unique.h>
#include <dingo/type_list.h>

#include <gtest/gtest.h>

#include "assert.h"
#include "class.h"
#include "containers.h"

namespace dingo {
template <typename T> struct shared_cyclical_test : public testing::Test {};
TYPED_TEST_SUITE(shared_cyclical_test, container_types);

TYPED_TEST(shared_cyclical_test, recursion_exception) {
    using container_type = TypeParam;

    struct B;
    struct A {
        A(B&) {}
    };
    struct B {
        B(std::shared_ptr<A>) {}
    };

    container_type container;
    container.template register_binding<storage<shared, std::shared_ptr<A>>>();
    container.template register_binding<storage<shared, B>>();

    ASSERT_THROW(container.template resolve<A>(), type_recursion_exception);
    ASSERT_THROW(container.template resolve<B>(), type_recursion_exception);
}

TYPED_TEST(shared_cyclical_test, value) {
    using container_type = TypeParam;

    struct B;
    struct shared_cyclical_value {};
    struct A : Class<shared_cyclical_value, __COUNTER__> {
        A(B& b) : b_(b) {}
        B& b_;
    };

    struct B : Class<shared_cyclical_value, __COUNTER__> {
        B(A& a, IClass1& ia) : a_(a), ia_(ia) {}
        A& a_;
        IClass1& ia_;
    };

    container_type container;
    container.template register_binding<storage<shared_cyclical, A, container_type>, A, IClass1>();
    container.template register_binding<storage<shared_cyclical, B, container_type>>();

    auto& a = container.template resolve<A&>();
    AssertClass(a);
    AssertClass(a.b_);

    auto& b = container.template resolve<B&>();
    AssertClass(b);
    AssertClass(b.a_);
    AssertClass(b.ia_);

    auto& c = container.template resolve<IClass1&>();
    AssertClass(c);
}

TYPED_TEST(shared_cyclical_test, shared_ptr) {
    using container_type = TypeParam;

    struct B;
    struct shared_cyclical_shared_ptr {};
    struct A : Class<shared_cyclical_shared_ptr, __COUNTER__> {
        A(B& b, IClass1& ib, std::shared_ptr<IClass1>& ibptr) : b_(b), ib_(ib), ibptr_(ibptr) {}
        B& b_;
        IClass1& ib_;
        std::shared_ptr<IClass1>& ibptr_;
    };

    struct B : Class<shared_cyclical_shared_ptr, __COUNTER__> {
        B(A& a, IClass2& ia, std::shared_ptr<IClass2>& iaptr) : a_(a), ia_(ia), iaptr_(iaptr) {}
        A& a_;
        IClass2& ia_;
        std::shared_ptr<IClass2>& iaptr_;
    };

    container_type container;
    container.template register_binding<storage<shared_cyclical, std::shared_ptr<A>, container_type>, A, IClass2>();
    container.template register_binding<storage<shared_cyclical, std::shared_ptr<B>, container_type>, B, IClass1>();

    auto& a = container.template resolve<A&>();
    AssertClass(a);
    AssertClass(a.b_);
    AssertClass(a.ib_);
    AssertClass(*a.ibptr_);

    auto& b = container.template resolve<B&>();
    AssertClass(b);
    AssertClass(b.a_);
    AssertClass(b.ia_);
    AssertClass(*b.iaptr_);
}

TYPED_TEST(shared_cyclical_test, trivially_destructible) {
    using container_type = TypeParam;

    struct B;
    struct A {
        B& b;
    };
    struct B {
        A& a;
    };

    container_type container;
    container.template register_binding<storage<shared_cyclical, A, container_type>>();
    container.template register_binding<storage<shared_cyclical, std::shared_ptr<B>, container_type>>();
    auto& a = container.template resolve<A&>();
    auto& b = container.template resolve<B&>();
    ASSERT_EQ(&a, &b.a);
    ASSERT_EQ(&b, &a.b);
}

TEST(shared_cyclical_test, virtual_base) {
    struct A {};
    struct B : A {};
    static_assert(!is_virtual_base_of_v<A, B>);

    struct C : virtual A {};
    static_assert(is_virtual_base_of_v<A, C>);
}

} // namespace dingo