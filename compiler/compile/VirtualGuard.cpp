/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 ******************************************************************************/

#include "compile/VirtualGuard.hpp"

#include <stdint.h>                            // for int16_t, int32_t
#include "codegen/CodeGenerator.hpp"           // for CodeGenerator
#include "codegen/FrontEnd.hpp"                // for TR_FrontEnd
#include "compile/Compilation.hpp"             // for Compilation
#include "compile/ResolvedMethod.hpp"          // for TR_ResolvedMethod
#include "compile/SymbolReferenceTable.hpp"    // for SymbolReferenceTable
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/CompilerEnv.hpp"
#include "env/jittypes.h"
#include "il/DataTypes.hpp"                    // for DataTypes::Int32, etc
#include "il/ILOpCodes.hpp"                    // for ILOpCodes::iconst, etc
#include "il/ILOps.hpp"                        // for ILOpCode
#include "il/Node.hpp"                         // for Node
#include "il/Node_inlines.hpp"                 // for Node::getSecondChild
#include "il/Symbol.hpp"                       // for Symbol
#include "il/SymbolReference.hpp"              // for SymbolReference
#include "il/symbol/MethodSymbol.hpp"          // for MethodSymbol
#include "il/symbol/ResolvedMethodSymbol.hpp"  // for ResolvedMethodSymbol
#include "infra/Assert.hpp"                    // for TR_ASSERT
#include "optimizer/PreExistence.hpp"          // for TR_InnerAssumption

#ifdef J9_PROJECT_SPECIFIC
#include "env/CHTable.hpp"                     // for TR_VirtualGuardSite
#endif

#ifdef RUBY_PROJECT_SPECIFIC
#include "ruby/config.h"
#endif

TR_VirtualGuard::TR_VirtualGuard(TR_VirtualGuardTestType test, TR_VirtualGuardKind kind,
             TR::Compilation * comp, TR::Node* callNode, TR::Node*guardNode,
             int16_t calleeIndex, int32_t currentSiteIndex, TR_OpaqueClassBlock *thisClass)
   : _test(test), _kind(kind), _guardedMethod((callNode && callNode->getOpCode().hasSymbolReference()) ? callNode->getSymbolReference() : NULL),
     _thisClass(thisClass), _callNode(0), _calleeIndex(calleeIndex), _cannotBeRemoved(false), _byteCodeIndex(0),
     _innerAssumptions(comp->trMemory()),
#ifdef J9_PROJECT_SPECIFIC
     _sites(comp->trMemory()),
#endif
     _mutableCallSiteObject(0),_mutableCallSiteEpoch(0),
     _evalChildren(true), _mergedWithHCRGuard(false),
     _guardNode(guardNode), _currentInlinedSiteIndex(currentSiteIndex)
   {
   if (callNode)
      {
      _bcInfo = callNode->getByteCodeInfo();
      }
   else
      {
      _bcInfo.setInvalidCallerIndex();
      _bcInfo.setInvalidByteCodeIndex();
      _bcInfo.setDoNotProfile(true);
      }
      
   comp->addVirtualGuard(this);
   if(kind != TR_ArrayStoreCheckGuard)
      {
      guardNode->setInlinedSiteIndex(calleeIndex);
      guardNode->setByteCodeIndex(0);
      }
   else
      _byteCodeIndex = callNode->getByteCodeInfo().getByteCodeIndex();
   if (comp->getOption(TR_TraceRelocatableDataDetailsCG))
      traceMsg(comp, "addVirtualGuard %p, guardkind = %d, virtualGuardTestType %d, bc index %d, callee index %d, callNode %p, guardNode %p, currentInlinedSiteIdx %d\n", this, _kind, test, this->getByteCodeIndex(), this->getCalleeIndex(), callNode, guardNode, _currentInlinedSiteIndex);
   }


TR_VirtualGuard::TR_VirtualGuard(TR_VirtualGuardTestType test, TR_VirtualGuardKind kind,
             TR::Compilation * comp, TR::Node* callNode, TR::Node*guardNode, int32_t currentSiteIndex)
   : _test(test), _kind(kind), _guardedMethod((callNode->getOpCode().hasSymbolReference()) ? callNode->getSymbolReference() : NULL),
     _thisClass(0), _callNode(callNode), _cannotBeRemoved(false), _mergedWithHCRGuard(false),
     _calleeIndex(callNode->getByteCodeInfo().getCallerIndex()),
     _byteCodeIndex(callNode->getByteCodeInfo().getByteCodeIndex()),
     _innerAssumptions(comp->trMemory()),
#ifdef J9_PROJECT_SPECIFIC
     _sites(comp->trMemory()),
#endif
     _guardNode(guardNode), _currentInlinedSiteIndex(currentSiteIndex)
   {
   if (kind==TR_SideEffectGuard)
      _callNode = 0;
   if (_callNode)
      {
      _bcInfo = _callNode->getByteCodeInfo();
      }
   else
      {
      _bcInfo.setInvalidCallerIndex();
      _bcInfo.setInvalidByteCodeIndex();
      _bcInfo.setDoNotProfile(true);
      }
      
   comp->addVirtualGuard(this);
   if (comp->getOption(TR_TraceRelocatableDataDetailsCG))
      traceMsg(comp, "addVirtualGuard %p, guardkind = %d, virtualGuardTestType %d, bc index %d, callee index %d, callNode %p, guardNode %p, currentInlinedSiteIdx %d\n", this, _kind, test, this->getByteCodeIndex(), this->getCalleeIndex(), callNode, guardNode, _currentInlinedSiteIndex);

   }

#ifdef J9_PROJECT_SPECIFIC
TR_VirtualGuardSite *
TR_VirtualGuard::addNOPSite()
   {
   TR_ASSERT(isNopable() || mergedWithHCRGuard(), "assertion failure");

   TR_VirtualGuardSite *site = new (trHeapMemory()) TR_VirtualGuardSite;

   _sites.add(site);
   return site;
   }
#endif

TR::Node*
TR_VirtualGuard::createRubyInlineGuard
(TR_VirtualGuardKind kind, TR::Compilation * comp, int16_t calleeIndex,
 TR::Node* callNode, TR::TreeTop * destination, TR_OpaqueClassBlock *thisClass)
{
  //Ensure that ILGen hasn't changed the children's layout we expect.
  TR_ASSERT(  ((callNode->getNumChildren() == 3) &&
       callNode->getSecondChild() &&
       callNode->getSecondChild()->getOpCodeValue() == TR::aconst), "Unexpected children in hierarchy of vm_send_without_block when creating ruby inline guard.");

  TR::Node* receiver = callNode->getThirdChild();
  TR::Node* callInfo = callNode->getSecondChild();

  //Call the ruby inline guard routine.
  TR::SymbolReferenceTable *symRefTab = comp->getSymRefTab();
  TR::SymbolReference *inline_guard_helperSymRef = symRefTab->findOrCreateRuntimeHelper(RubyHelper_vm_send_woblock_inlineable_guard, true, true, false);

  TR::Node* isInlineable = TR::Node::create(TR::lcall, 2);
  isInlineable->setSymbolReference(inline_guard_helperSymRef);
  isInlineable->setAndIncChild(0, TR::Node::aconst(callInfo, (uintptrj_t)callInfo->getAddress()));
  isInlineable->setAndIncChild(1, receiver);

  TR::Node* guard = TR::Node::createif(TR::ificmpne,
					 isInlineable,
					 TR::Node::iconst(callNode, (uintptrj_t)1),
					 destination);

  setGuardKind(guard, kind, comp);

  TR_VirtualGuard *vg = new (comp->trHeapMemory()) TR_VirtualGuard(TR_RubyInlineTest, kind, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex(), thisClass);
  return guard;
}

TR::Node*
TR_VirtualGuard::createVftGuard
(TR_VirtualGuardKind kind, TR::Compilation * comp, int16_t calleeIndex,
 TR::Node* callNode, TR::TreeTop * destination, TR_OpaqueClassBlock *thisClass)
   {
   TR::SymbolReferenceTable *symRefTab = comp->getSymRefTab();

   TR::Node* vft = TR::Node::createWithSymRef(TR::aloadi, 1, 1, callNode->getSecondChild(), symRefTab->findOrCreateVftSymbolRef());
   TR::Node* aconstNode = TR::Node::aconst(callNode, (uintptrj_t)thisClass);
   aconstNode->setIsClassPointerConstant(true);
   aconstNode->setInlinedSiteIndex(calleeIndex);
   aconstNode->setByteCodeIndex(0);

   TR::Node*guard = TR::Node::createif(TR::ifacmpne, vft, aconstNode, destination);
   setGuardKind(guard, kind, comp);

   TR_VirtualGuard *vg = new (comp->trHeapMemory()) TR_VirtualGuard(TR_VftTest, kind, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex(), thisClass);
   //traceMsg(comp, "guard %p virtualguard %p\n", guard, vg);

   if (comp->compileRelocatableCode())
      vg->setCannotBeRemoved();

   return guard;
   }

TR::Node*
TR_VirtualGuard::createMethodGuard
(TR_VirtualGuardKind kind, TR::Compilation * comp, int16_t calleeIndex,
 TR::Node* callNode, TR::TreeTop * destination, TR::ResolvedMethodSymbol * calleeSymbol, TR_OpaqueClassBlock *thisClass)
   {
   return createMethodGuardWithReceiver (kind, comp, calleeIndex, callNode, destination, calleeSymbol, thisClass, callNode->getSecondChild());
   }

TR::Node*
TR_VirtualGuard::createMethodGuardWithReceiver
(TR_VirtualGuardKind kind, TR::Compilation * comp, int16_t calleeIndex,
 TR::Node* callNode, TR::TreeTop * destination, TR::ResolvedMethodSymbol * calleeSymbol, TR_OpaqueClassBlock *thisClass, TR::Node* receiverNode)
   {
#ifdef J9_PROJECT_SPECIFIC
   TR::SymbolReferenceTable * symRefTab = comp->getSymRefTab();
   TR::Node* vft = TR::Node::createWithSymRef(TR::aloadi, 1, 1, receiverNode, symRefTab->findOrCreateVftSymbolRef());
   TR::SymbolReference * callSymRef = callNode->getSymbolReference();

   int32_t offset;
   TR_OpaqueClassBlock *realThisClass = thisClass;
   if (thisClass && TR::Compiler->cls.isInterfaceClass(comp, thisClass) && callSymRef->getSymbol()->castToMethodSymbol()->isInterface())
      realThisClass = calleeSymbol->getResolvedMethod()->classOfMethod();

   if (realThisClass && !TR::Compiler->cls.isInterfaceClass(comp, realThisClass) && callSymRef->getSymbol()->castToMethodSymbol()->isInterface())
      offset = callSymRef->getOwningMethod(comp)->getResolvedInterfaceMethodOffset(realThisClass, callSymRef->getCPIndex());
   else
      offset = (int32_t) callSymRef->getOffset();

   TR::Node* vftEntry =
      TR::Node::createWithSymRef(TR::aloadi, 1, 1, vft,
            symRefTab->findOrCreateVtableEntrySymbolRef(calleeSymbol,
                         comp->fej9()->virtualCallOffsetToVTableSlot(offset)));

   TR::Node * aconstNode = TR::Node::aconst(callNode, (uintptrj_t)calleeSymbol->getResolvedMethod()->getPersistentIdentifier());
   aconstNode->setIsMethodPointerConstant(true);
   aconstNode->setInlinedSiteIndex(calleeIndex);
   aconstNode->setByteCodeIndex(0);

   TR::Node *guard = TR::Node::createif(TR::ifacmpne, vftEntry, aconstNode, destination);
   setGuardKind(guard, kind, comp);

   TR_VirtualGuard *vg = new (comp->trHeapMemory()) TR_VirtualGuard(TR_MethodTest, kind, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex(), thisClass);

   if (comp->compileRelocatableCode())
      vg->setCannotBeRemoved();

   return guard;
#else
   return NULL;
#endif
   }

TR::Node *
TR_VirtualGuard::createNonoverriddenGuard
(TR_VirtualGuardKind kind, TR::Compilation * comp, int16_t calleeIndex,
 TR::Node * callNode, TR::TreeTop * destination, TR::ResolvedMethodSymbol * calleeSymbol, bool forInline)
   {
   TR::SymbolReferenceTable * symRefTab = comp->getSymRefTab();
   TR::SymbolReference * addressSymRef = symRefTab->createIsOverriddenSymbolRef(calleeSymbol);

   TR::Node * guard = NULL;
   if (TR::Compiler->target.is64Bit())
      {
      TR::Node * load = TR::Node::createWithSymRef(callNode, TR::lload, 0, addressSymRef);
      TR::Node * flagBit = TR::Node::create(callNode, TR::lconst, 0, 0);
      flagBit->setLongInt(comp->fe()->offsetOfIsOverriddenBit());

      guard =  TR::Node::createif(TR::iflcmpne,
                        TR::Node::create(TR::land, 2, load, flagBit),
               TR::Node::create(callNode, TR::lconst),
               destination);
      }
   else
      {
      TR::Node * load = TR::Node::createWithSymRef(callNode, TR::iload, 0, addressSymRef);
      TR::Node * flagBit = TR::Node::create(callNode, TR::iconst, 0, comp->fe()->offsetOfIsOverriddenBit());

      guard =  TR::Node::createif(TR::ificmpne,
               TR::Node::create(TR::iand, 2, load, flagBit),
               TR::Node::create(callNode, TR::iconst),
               destination);
      }

   setGuardKind(guard, kind, comp);

   TR_VirtualGuard *vg = (TR_VirtualGuard*)(new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, kind, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex()));

   if (!forInline)
      {
      vg->_byteCodeIndex = callNode->getByteCodeIndex();
      guard->setByteCodeIndex(vg->_byteCodeIndex);
      }

   if (comp->compileRelocatableCode())
      vg->setCannotBeRemoved();

   if (comp->cg()->getSupportsVirtualGuardNOPing())
      vg->dontGenerateChildrenCode();

   return guard;
   }


TR::Node *
TR_VirtualGuard::createMutableCallSiteTargetGuard(TR::Compilation * comp, int16_t calleeIndex, TR::Node * node, TR::TreeTop * destination, uintptrj_t *mcsObject, TR::KnownObjectTable::Index mcsEpoch)
   {
   TR::SymbolReferenceTable *symRefTab = comp->getSymRefTab();
   TR::SymbolReference *addressSymRef = symRefTab->createKnownStaticDataSymbolRef(0, TR::Address, mcsEpoch);
   addressSymRef->setSideEffectInfo();
   TR::Node *receiver = node->getArgument(0);
   TR::Node *load     = TR::Node::createWithSymRef(node, TR::aload,  0, addressSymRef);
   TR::Node *guard    = TR::Node::createif(TR::ifacmpne, node, load, destination); // NOTE: take bytecode info from node so findVirtualGuardInfo works
   guard->getAndDecChild(0);
   guard->setAndIncChild(0, receiver);
   setGuardKind(guard, TR_MutableCallSiteTargetGuard, comp);

   TR_VirtualGuard *vguard=new (comp->trHeapMemory()) TR_VirtualGuard(TR_DummyTest, TR_MutableCallSiteTargetGuard, comp, node, guard, comp->getCurrentInlinedSiteIndex());
   vguard->_mutableCallSiteObject = mcsObject;
   vguard->_mutableCallSiteEpoch  = mcsEpoch;
   vguard->dontGenerateChildrenCode();
   return guard;
   }


TR::Node *
TR_VirtualGuard::createDummyOrSideEffectGuard(TR::Compilation * comp, TR::Node * callNode, TR::TreeTop *destination)
   {
   TR::SymbolReferenceTable *symRefTab = comp->getSymRefTab();
   TR::SymbolReference *addressSymRef = symRefTab->createKnownStaticDataSymbolRef(0, TR::Int32);
   addressSymRef->setSideEffectInfo();
   TR::Node *load  = TR::Node::createWithSymRef(callNode, TR::iload,  0, addressSymRef);
   TR::Node *flag  = TR::Node::create(callNode, TR::iconst, 0, 0);
   TR::Node *guard = TR::Node::createif(TR::ificmpne,
                  load, flag, destination);
   return guard;
   }

TR::Node *
TR_VirtualGuard::createSideEffectGuard(TR::Compilation * comp, TR::Node * callNode, TR::TreeTop *destination)
   {
   TR::Node *guard = createDummyOrSideEffectGuard(comp, callNode, destination);
   setGuardKind(guard, TR_SideEffectGuard, comp);
   TR_VirtualGuard *vguard=new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, TR_SideEffectGuard, comp, callNode, guard, comp->getCurrentInlinedSiteIndex());
   vguard->dontGenerateChildrenCode();
   return guard;
   }

TR::Node *
TR_VirtualGuard::createAOTInliningGuard(TR::Compilation *comp, int16_t calleeIndex, TR::Node *callNode, TR::TreeTop *destination, TR_VirtualGuardKind kind)
   {
   TR::Node *guard = createDummyOrSideEffectGuard(comp, callNode, destination);
   setGuardKind(guard, kind, comp);
   TR_VirtualGuard *vguard=new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, kind, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex());
   vguard->dontGenerateChildrenCode();
   vguard->setCannotBeRemoved();
   return guard;
   }

TR::Node *
TR_VirtualGuard::createAOTGuard(TR::Compilation *comp, int16_t siteIndex, TR::Node *callNode, TR::TreeTop *destination, TR_VirtualGuardKind kind)
   {
   TR::Node *guard = createDummyOrSideEffectGuard(comp, callNode, destination);
   setGuardKind(guard, kind, comp);
   TR_VirtualGuard *vguard=new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, kind, comp, callNode, guard, siteIndex);
   vguard->dontGenerateChildrenCode();
   vguard->setCannotBeRemoved();
   return guard;
   }

TR::Node *
TR_VirtualGuard::createDummyGuard(TR::Compilation *comp, int16_t calleeIndex, TR::Node *callNode, TR::TreeTop *destination)
   {
   TR::Node *guard = createDummyOrSideEffectGuard(comp, callNode, destination);
   setGuardKind(guard, TR_DummyGuard, comp);
   TR_VirtualGuard *vguard=new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, TR_DummyGuard, comp, callNode, guard, calleeIndex, comp->getCurrentInlinedSiteIndex());
   vguard->dontGenerateChildrenCode();
   if (comp->compileRelocatableCode())
      vguard->setCannotBeRemoved();

   return guard;
   }

TR_VirtualGuard *
TR_VirtualGuard::createGuardedDevirtualizationGuard
(TR_VirtualGuardKind kind, TR::Compilation * comp, TR::Node * callNode)
   {
   return new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, kind, comp, callNode, NULL, comp->getCurrentInlinedSiteIndex());
   }

TR_VirtualGuard *
TR_VirtualGuard::createArrayStoreCheckGuard(TR::Compilation *comp, TR::Node *ascNode, TR_OpaqueClassBlock *clazz)
   {
   return new (comp->trHeapMemory()) TR_VirtualGuard(TR_VftTest, TR_ArrayStoreCheckGuard, comp, ascNode, NULL,
         ascNode->getByteCodeInfo().getCallerIndex(), comp->getCurrentInlinedSiteIndex(), clazz);
   }

 TR::Node *
TR_VirtualGuard::createOSRGuard(TR::Compilation *comp, TR::TreeTop *destination)
   {
   TR::SymbolReferenceTable * symRefTab = comp->getSymRefTab();
   TR::SymbolReference * addressSymRef = symRefTab->createKnownStaticDataSymbolRef(0, TR::Int32);
   addressSymRef->setSideEffectInfo();
   TR::Node * load = destination ? TR::Node::createWithSymRef(destination->getNode(), TR::iload, 0, addressSymRef) : TR::Node::createWithSymRef(TR::iload, 0, addressSymRef);
   TR::Node * flag = destination ? TR::Node::create(destination->getNode(), TR::iconst, 0, 0) : TR::Node::create(TR::iconst, 0, 0);

   TR::Node * guard =  TR::Node::createif(TR::ificmpne,
               load,
               flag,
               destination);
   setGuardKind(guard, TR_OSRGuard, comp);

   TR_VirtualGuard *vguard = new (comp->trHeapMemory()) TR_VirtualGuard(TR_DummyTest, TR_OSRGuard, comp,
         NULL, guard, destination ? destination->getNode()->getByteCodeInfo().getCallerIndex() : -1, comp->getCurrentInlinedSiteIndex(), NULL);
   vguard->dontGenerateChildrenCode();
   return guard;
   }

TR::Node *
TR_VirtualGuard::createHCRGuard
(TR::Compilation * comp, int16_t calleeIndex, TR::Node * node, TR::TreeTop * destination, TR::ResolvedMethodSymbol * symbol, TR_OpaqueClassBlock *thisClass)
   {
   TR::SymbolReferenceTable * symRefTab = comp->getSymRefTab();
   TR::SymbolReference * addressSymRef = symRefTab->createKnownStaticDataSymbolRef(0, TR::Int32);
   addressSymRef->setSideEffectInfo();
   TR::Node * load = TR::Node::createWithSymRef(node, TR::iload, 0, addressSymRef);
   TR::Node * flag = TR::Node::create(node, TR::iconst, 0, 0);

   TR::Node * guard =  TR::Node::createif(TR::ificmpne,
               load,
               flag,
               destination);
   setGuardKind(guard, TR_HCRGuard, comp);

   TR_VirtualGuard *vguard = new (comp->trHeapMemory()) TR_VirtualGuard(TR_NonoverriddenTest, TR_HCRGuard, comp, node, guard, calleeIndex, comp->getCurrentInlinedSiteIndex(), thisClass);
   vguard->dontGenerateChildrenCode();

   return guard;
   }

void
TR_VirtualGuard::addInnerAssumption(TR::Compilation *comp, int32_t ordinal, TR_VirtualGuard *guard)
   { addInnerAssumption(new (comp->trHeapMemory()) TR_InnerAssumption(ordinal, guard)); }

void
TR_VirtualGuard::addInnerAssumption(TR_InnerAssumption *a)
   {
   _cannotBeRemoved  = true;
   _innerAssumptions.add(a);
   }

void
TR_VirtualGuard::setGuardKind(TR::Node * node, TR_VirtualGuardKind kind, TR::Compilation * comp)
   {
   switch (kind)
      {
      case TR_ProfiledGuard:
         node->setIsProfiledGuard();
         break;
      case TR_InterfaceGuard:
         node->setIsInterfaceGuard();
         break;
      case TR_AbstractGuard:
         node->setIsAbstractGuard();
         break;
      case TR_HierarchyGuard:
         node->setIsHierarchyGuard();
         break;
      case TR_DummyGuard:
         node->setIsDummyGuard();
         break;
      case TR_HCRGuard:
         node->setIsHCRGuard();
         break;
      case TR_SideEffectGuard:
         node->setIsSideEffectGuard();
         break;
      case TR_MutableCallSiteTargetGuard:
         node->setIsMutableCallSiteTargetGuard();
         break;
      case TR_MethodEnterExitGuard:
         node->setIsMethodEnterExitGuard();
         break;
      case TR_DirectMethodGuard:
         node->setIsDirectMethodGuard();
         break;
      case TR_OSRGuard:
         node->setIsOSRGuard();
         break;
      default:
         TR_ASSERT(kind == TR_NonoverriddenGuard, "Expected TR_NonoverriddenGuard(%d); found %d", (int)TR_NonoverriddenGuard, kind);
         node->setIsNonoverriddenGuard();
         break;
      }
   }
