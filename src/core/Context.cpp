// Context.cpp (Oclgrind)
// Copyright (c) 2013-2014, James Price and Simon McIntosh-Smith,
// University of Bristol. All rights reserved.
//
// This program is provided under a three-clause BSD license. For full
// license terms please see the LICENSE file distributed with this
// source code.

#include "common.h"

#if defined(_WIN32) && !defined(__MINGW32__)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "llvm/DebugInfo.h"
#include "llvm/Instruction.h"

#include "Context.h"
#include "Kernel.h"
#include "KernelInvocation.h"
#include "Memory.h"
#include "WorkGroup.h"
#include "WorkItem.h"

#include "plugins/InstructionCounter.h"
#include "plugins/InteractiveDebugger.h"
#include "plugins/Logger.h"
#include "plugins/RaceDetector.h"

using namespace oclgrind;
using namespace std;

Context::Context()
{
  m_globalMemory = new Memory(AddrSpaceGlobal, this);
  m_kernelInvocation = NULL;

  loadPlugins();
}

Context::~Context()
{
  delete m_globalMemory;

  unloadPlugins();
}

Memory* Context::getGlobalMemory() const
{
  return m_globalMemory;
}

void Context::loadPlugins()
{
  // Create core plugins
  m_plugins.push_back(make_pair(new Logger(this), true));

  const char *instCounts = getenv("OCLGRIND_INST_COUNTS");
  if (instCounts && strcmp(instCounts, "1") == 0)
    m_plugins.push_back(make_pair(new InstructionCounter(this), true));

  const char *dataRaces = getenv("OCLGRIND_DATA_RACES");
  if (dataRaces && strcmp(dataRaces, "1") == 0)
    m_plugins.push_back(make_pair(new RaceDetector(this), true));

  const char *interactive = getenv("OCLGRIND_INTERACTIVE");
  if (interactive && strcmp(interactive, "1") == 0)
    m_plugins.push_back(make_pair(new InteractiveDebugger(this), true));


  // Load dynamic plugins
  const char *dynamicPlugins = getenv("OCLGRIND_PLUGINS");
  if (dynamicPlugins)
  {
    std::istringstream ss(dynamicPlugins);
    std::string libpath;
    while(std::getline(ss, libpath, ':'))
    {
#if defined(_WIN32) && !defined(__MINGW32__)
      HMODULE library = LoadLibrary(libpath.c_str());
      if (!library)
      {
        cerr << "Loading Oclgrind plugin failed (LoadLibrary): "
             << GetLastError() << endl;
        continue;
      }

      void *initialize = GetProcAddress(library, "initializePlugins");
      if (!initialize)
      {
        cerr << "Loading Oclgrind plugin failed (GetProcAddress): "
             << GetLastError() << endl;
        continue;
      }
#else
      void *library = dlopen(libpath.c_str(), RTLD_NOW);
      if (!library)
      {
        cerr << "Loading Oclgrind plugin failed (dlopen): "
             << dlerror() << endl;
        continue;
      }

      void *initialize = dlsym(library, "initializePlugins");
      if (!initialize)
      {
        cerr << "Loading Oclgrind plugin failed (dlsym): "
             << dlerror() << endl;
        continue;
      }
#endif

      ((void(*)(Context*))initialize)(this);
      m_pluginLibraries.push_back(library);
    }
  }
}

void Context::unloadPlugins()
{
  // Release dynamic plugin libraries
  list<void*>::iterator plibItr;
  for (plibItr = m_pluginLibraries.begin();
       plibItr != m_pluginLibraries.end(); plibItr++)
  {
#if defined(_WIN32) && !defined(__MINGW32__)
      void *release = GetProcAddress(*plibItr, "releasePlugins");
      if (release)
      {
        ((void(*)(Context*))release)(this);
      }
      FreeLibrary(*plibItr);
#else
      void *release = dlsym(*plibItr, "releasePlugins");
      if (release)
      {
        ((void(*)(Context*))release)(this);
      }
      dlclose(*plibItr);
#endif
  }

  // Destroy internal plugins
  PluginList::iterator pItr;
  for (pItr = m_plugins.begin(); pItr != m_plugins.end(); pItr++)
  {
    if (pItr->second)
      delete pItr->first;
  }

  m_plugins.clear();
}

void Context::registerPlugin(Plugin *plugin)
{
  m_plugins.push_back(make_pair(plugin, false));
}

void Context::unregisterPlugin(Plugin *plugin)
{
  m_plugins.remove(make_pair(plugin, false));
}

void Context::logError(const char* error, const char* info) const
{
  // Error info
  cerr << endl << error << endl;
  printErrorContext();
  if (info)
  {
    cerr << "\t" << info << endl;
  }
  cerr << endl;

  // TODO: THIS (notifyError)
  //m_device->forceBreak();
}

void Context::logMemoryError(bool read, unsigned int addrSpace,
                             size_t address, size_t size) const
{
  string memType;
  switch (addrSpace)
  {
  case AddrSpacePrivate:
    memType = "private";
    break;
  case AddrSpaceGlobal:
    memType = "global";
    break;
  case AddrSpaceConstant:
    memType = "constant";
    break;
  case AddrSpaceLocal:
    memType = "local";
    break;
  default:
    assert(false && "Memory error in unsupported address space.");
    break;
  }

  // Error info
  cerr << endl << "Invalid " << (read ? "read" : "write")
       << " of size " << size
       << " at " << memType
       << " memory address " << hex << address << endl;

  printErrorContext();
  cerr << endl;

  // TODO: THIS (notifyError)
  //m_device->forceBreak();
}

void Context::printErrorContext() const
{
  // Work item
  const WorkItem *workItem = m_kernelInvocation->getCurrentWorkItem();
  if (workItem)
  {
    cerr << "\tWork-item: "
         << " Global" << workItem->getGlobalID()
         << " Local" << workItem->getLocalID() << endl;
  }

  // Work group
  const WorkGroup *workGroup = m_kernelInvocation->getCurrentWorkGroup();
  if (workGroup)
  {
    Size3 group = workGroup->getGroupID();
    cerr << "\tWork-group: " << group << endl;
  }

  // Kernel
  const Kernel *kernel = m_kernelInvocation->getKernel();
  if (kernel)
  {
    cerr << "\tKernel:     " << kernel->getName() << endl;
  }

  // Instruction
  if (workItem)
  {
    cerr << "\t";
    printInstruction(workItem->getCurrentInstruction());
  }
}

void Context::printInstruction(const llvm::Instruction *instruction) const
{
  dumpInstruction(cerr, instruction);
  cerr << endl;

  // Output debug information
  cerr << "\t";
  llvm::MDNode *md = instruction->getMetadata("dbg");
  if (!md)
  {
    cerr << "Debugging information not available." << endl;
  }
  else
  {
    llvm::DILocation loc(md);
    cerr << "At line " << dec << loc.getLineNumber()
         << " of " << loc.getFilename().str() << endl;
  }
}

#define NOTIFY(function, ...)                     \
  PluginList::const_iterator pluginItr;           \
  for (pluginItr = m_plugins.begin();             \
       pluginItr != m_plugins.end(); pluginItr++) \
  {                                               \
    pluginItr->first->function(__VA_ARGS__);      \
  }

void Context::notifyInstructionExecuted(const WorkItem *workItem,
                                        const llvm::Instruction *instruction,
                                        const TypedValue& result) const
{
  NOTIFY(instructionExecuted, workItem, instruction, result);
}

void Context::notifyKernelBegin(KernelInvocation *kernelInvocation) const
{
  assert(m_kernelInvocation == NULL);
  m_kernelInvocation = kernelInvocation;

  NOTIFY(kernelBegin, kernelInvocation);
}

void Context::notifyKernelEnd(KernelInvocation *kernelInvocation) const
{
  NOTIFY(kernelEnd, kernelInvocation);

  assert(m_kernelInvocation == kernelInvocation);
  m_kernelInvocation = NULL;
}

void Context::notifyMemoryAllocated(const Memory *memory, size_t address,
                                    size_t size) const
{
  NOTIFY(memoryAllocated, memory, address, size);
}

void Context::notifyMemoryAtomic(const Memory *memory, size_t address,
                                 size_t size) const
{
  NOTIFY(memoryAtomic, memory, address, size);
}

void Context::notifyMemoryDeallocated(const Memory *memory,
                                      size_t address) const
{
  NOTIFY(memoryDeallocated, memory, address);
}

void Context::notifyMemoryLoad(const Memory *memory, size_t address,
                               size_t size) const
{
  NOTIFY(memoryLoad, memory, address, size);
}

void Context::notifyMemoryStore(const Memory *memory, size_t address,
                                size_t size, const uint8_t *storeData) const
{
  NOTIFY(memoryStore, memory, address, size, storeData);
}

void Context::notifyMessage(MessageType type, const char *message) const
{
  NOTIFY(log, type, message);
}

void Context::notifyWorkGroupBarrier(const WorkGroup *workGroup,
                                     uint32_t flags) const
{
  NOTIFY(workGroupBarrier, workGroup, flags);
}

#undef NOTIFY


Context::Message::Message(MessageType type, const Context *context)
{
  m_type             = type;
  m_context          = context;
  m_kernelInvocation = context->m_kernelInvocation;
}

Context::Message& Context::Message::operator<<(const special& id)
{
  switch (id)
  {
  case INDENT:
    m_indentModifiers.push_back( m_stream.tellp());
    break;
  case UNINDENT:
    m_indentModifiers.push_back(-m_stream.tellp());
    break;
  case CURRENT_KERNEL:
    *this << m_kernelInvocation->getKernel()->getName();
    break;
  case CURRENT_WORK_ITEM_GLOBAL:
  {
    const WorkItem *workItem = m_kernelInvocation->getCurrentWorkItem();
    if (workItem)
    {
      *this << workItem->getGlobalID();
    }
    else
    {
      *this << "(none)";
    }
    break;
  }
  case CURRENT_WORK_ITEM_LOCAL:
  {
    const WorkItem *workItem = m_kernelInvocation->getCurrentWorkItem();
    if (workItem)
    {
      *this << workItem->getLocalID();
    }
    else
    {
      *this << "(none)";
    }
    break;
  }
  case CURRENT_WORK_GROUP:
  {
    const WorkGroup *workGroup = m_kernelInvocation->getCurrentWorkGroup();
    if (workGroup)
    {
      *this << workGroup->getGroupID();
    }
    else
    {
      *this << "(none)";
    }
    break;
  }
  case CURRENT_ENTITY:
  {
    const WorkItem *workItem = m_kernelInvocation->getCurrentWorkItem();
    const WorkGroup *workGroup = m_kernelInvocation->getCurrentWorkGroup();
    if (workItem)
    {
      *this << "Global" << workItem->getGlobalID()
            << " Local" << workItem->getLocalID() << " ";
    }
    if (workGroup)
    {
      *this << "Group" << workGroup->getGroupID();
    }
    if (!workItem && ! workGroup)
    {
      *this << "(unknown)";
    }
    break;
  }
  case CURRENT_LOCATION:
  {
    const llvm::Instruction *instruction = NULL;
    const WorkItem *workItem = m_kernelInvocation->getCurrentWorkItem();
    if (workItem)
    {
      instruction = workItem->getCurrentInstruction();
    }
    // TODO: Attempt to get work-group location (e.g. barrier)

    *this << instruction;
    break;
  }
  }
  return *this;
}

Context::Message& Context::Message::operator<<(
  const llvm::Instruction *instruction)
{
  if (instruction)
  {
    // Output instruction
    dumpInstruction(m_stream, instruction);
    *this << endl;

    // Output debug information
    llvm::MDNode *md = instruction->getMetadata("dbg");
    if (!md)
    {
      *this << "Debugging information not available." << endl;
    }
    else
    {
      // TODO: Show line of source code
      llvm::DILocation loc(md);
      *this << "At line " << dec << loc.getLineNumber()
           << " of " << loc.getFilename().str();
    }
  }
  else
  {
    *this << "(location unknown)";
  }

  return *this;
}

Context::Message& Context::Message::operator<<(
  std::ostream& (*t)(std::ostream&))
{
  m_stream << t;
  return *this;
}

Context::Message& Context::Message::operator<<(
  std::ios& (*t)(std::ios&))
{
  m_stream << t;
  return *this;
}

Context::Message& Context::Message::operator<<(
  std::ios_base& (*t)(std::ios_base&))
{
  m_stream << t;
  return *this;
}

void Context::Message::send() const
{
  string msg;

  string line;
  int currentIndent = 0;
  list<int>::const_iterator itr = m_indentModifiers.begin();

  m_stream.clear();
  m_stream.seekg(0);
  while (m_stream.good())
  {
    getline(m_stream, line);

    // TODO: Wrap long lines
    msg += line;

    // Check for indentation modifiers
    long pos = m_stream.tellg();
    if (itr != m_indentModifiers.end() && pos >= abs(*itr))
    {
      if (*itr >= 0)
        currentIndent++;
      else
        currentIndent--;
      itr++;
    }

    if (!m_stream.eof())
    {
      // Add newline and indentation
      msg += '\n';
      for (int i = 0; i < currentIndent; i++)
        msg += '\t';
    }
  }

  m_context->notifyMessage(m_type, msg.c_str());
}
