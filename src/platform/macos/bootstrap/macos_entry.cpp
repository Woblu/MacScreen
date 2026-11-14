// Library entry points. This file is linked last so its initializer runs after
// all other static initializers in Toolscreen, as DllMain runs on Windows.

void MacDllAttach();
void MacDllDetach();

namespace {

__attribute__((constructor)) void ToolscreenMacAttach() { MacDllAttach(); }

__attribute__((destructor)) void ToolscreenMacDetach() { MacDllDetach(); }

} // namespace
