// SPDX-License-Identifier: MIT
//
// ADR-040: the built-in Field/Form components and a page using them compile in base mode: the
// generated form-state binding, the typed edit events and the validate/submit bindings all reach
// developer classes. Every member of every template instantiates (explicit instantiation).

#include "Field.hpp"
#include "FieldPage.hpp"
#include "Form.hpp"
#include "FormPage.hpp"

namespace demo {

template struct gen::FieldBase<Field>;
template struct gen::FieldPageBase<FieldPage>;
template struct gen::FormBase<Form>;
template struct gen::FormPageBase<FormPage>;

} // namespace demo
