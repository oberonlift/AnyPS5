#include <ps5lift/State.h>

namespace {

const Ps5LiftProgram EmptyProgram = {.GuestBase = PS5LIFT_GUEST_BASE};
const bool Used = (ps5lift_use_program(&EmptyProgram), true);

}
