/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/RestyleManager.h"

#include "mozilla/AutoRestyleTimelineMarker.h"
#include "mozilla/AutoTimelineMarker.h"
#include "mozilla/ComputedStyle.h"
#include "mozilla/ComputedStyleInlines.h"
#include "mozilla/DocumentStyleRootIterator.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoStyleSetInlines.h"
#include "mozilla/Unused.h"
#include "mozilla/ViewportFrame.h"
#include "mozilla/dom/ChildIterator.h"
#include "mozilla/dom/ElementInlines.h"

#include "Layers.h"
#include "LayerAnimationInfo.h" // For LayerAnimationInfo::sRecords
#include "nsAnimationManager.h"
#include "nsBlockFrame.h"
#include "nsBulletFrame.h"
#include "nsContentUtils.h"
#include "nsCSSFrameConstructor.h"
#include "nsCSSRendering.h"
#include "nsIFrame.h"
#include "nsIFrameInlines.h"
#include "nsImageFrame.h"
#include "nsIPresShellInlines.h"
#include "nsPlaceholderFrame.h"
#include "nsPrintfCString.h"
#include "nsRefreshDriver.h"
#include "nsStyleChangeList.h"
#include "nsStyleUtil.h"
#include "nsTransitionManager.h"
#include "StickyScrollContainer.h"
#include "mozilla/EffectSet.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/ViewportFrame.h"
#include "SVGObserverUtils.h"
#include "SVGTextFrame.h"
#include "ActiveLayerTracker.h"
#include "nsSVGIntegrationUtils.h"

#ifdef ACCESSIBILITY
#include "nsAccessibilityService.h"
#endif

using namespace mozilla::dom;

namespace mozilla {

RestyleManager::RestyleManager(nsPresContext* aPresContext)
  : mPresContext(aPresContext)
  , mRestyleGeneration(1)
  , mUndisplayedRestyleGeneration(1)
  , mInStyleRefresh(false)
  , mAnimationGeneration(0)
{
  MOZ_ASSERT(mPresContext);
}

void
RestyleManager::ContentInserted(nsIContent* aChild)
{
  MOZ_ASSERT(aChild->GetParentNode());
  RestyleForInsertOrChange(aChild);
}

void
RestyleManager::ContentAppended(nsIContent* aFirstNewContent)
{
  MOZ_ASSERT(aFirstNewContent->GetParent());

  // The container cannot be a document, but might be a ShadowRoot.
  if (!aFirstNewContent->GetParentNode()->IsElement()) {
    return;
  }
  Element* container = aFirstNewContent->GetParentNode()->AsElement();

#ifdef DEBUG
  {
    for (nsIContent* cur = aFirstNewContent; cur; cur = cur->GetNextSibling()) {
      NS_ASSERTION(!cur->IsRootOfAnonymousSubtree(),
                   "anonymous nodes should not be in child lists");
    }
  }
#endif
  uint32_t selectorFlags =
    container->GetFlags() & (NODE_ALL_SELECTOR_FLAGS &
                             ~NODE_HAS_SLOW_SELECTOR_LATER_SIBLINGS);
  if (selectorFlags == 0)
    return;

  if (selectorFlags & NODE_HAS_EMPTY_SELECTOR) {
    // see whether we need to restyle the container
    bool wasEmpty = true; // :empty or :-moz-only-whitespace
    for (nsIContent* cur = container->GetFirstChild();
         cur != aFirstNewContent;
         cur = cur->GetNextSibling()) {
      // We don't know whether we're testing :empty or :-moz-only-whitespace,
      // so be conservative and assume :-moz-only-whitespace (i.e., make
      // IsSignificantChild less likely to be true, and thus make us more
      // likely to restyle).
      if (nsStyleUtil::IsSignificantChild(cur, false)) {
        wasEmpty = false;
        break;
      }
    }
    if (wasEmpty) {
      RestyleForEmptyChange(container);
      return;
    }
  }

  if (selectorFlags & NODE_HAS_SLOW_SELECTOR) {
    PostRestyleEvent(container, eRestyle_Subtree, nsChangeHint(0));
    // Restyling the container is the most we can do here, so we're done.
    return;
  }

  if (selectorFlags & NODE_HAS_EDGE_CHILD_SELECTOR) {
    // restyle the last element child before this node
    for (nsIContent* cur = aFirstNewContent->GetPreviousSibling();
         cur;
         cur = cur->GetPreviousSibling()) {
      if (cur->IsElement()) {
        PostRestyleEvent(cur->AsElement(), eRestyle_Subtree, nsChangeHint(0));
        break;
      }
    }
  }
}

void
RestyleManager::RestyleForEmptyChange(Element* aContainer)
{
  // In some cases (:empty + E, :empty ~ E), a change in the content of
  // an element requires restyling its parent's siblings.
  nsRestyleHint hint = eRestyle_Subtree;
  nsIContent* grandparent = aContainer->GetParent();
  if (grandparent &&
      (grandparent->GetFlags() & NODE_HAS_SLOW_SELECTOR_LATER_SIBLINGS)) {
    hint = nsRestyleHint(hint | eRestyle_LaterSiblings);
  }
  PostRestyleEvent(aContainer, hint, nsChangeHint(0));
}

void
RestyleManager::MaybeRestyleForEdgeChildChange(Element* aContainer,
                                               nsIContent* aChangedChild)
{
  MOZ_ASSERT(aContainer->GetFlags() & NODE_HAS_EDGE_CHILD_SELECTOR);
  MOZ_ASSERT(aChangedChild->GetParent() == aContainer);
  // restyle the previously-first element child if it is after this node
  bool passedChild = false;
  for (nsIContent* content = aContainer->GetFirstChild();
       content;
       content = content->GetNextSibling()) {
    if (content == aChangedChild) {
      passedChild = true;
      continue;
    }
    if (content->IsElement()) {
      if (passedChild) {
        PostRestyleEvent(content->AsElement(), eRestyle_Subtree,
                         nsChangeHint(0));
      }
      break;
    }
  }
  // restyle the previously-last element child if it is before this node
  passedChild = false;
  for (nsIContent* content = aContainer->GetLastChild();
       content;
       content = content->GetPreviousSibling()) {
    if (content == aChangedChild) {
      passedChild = true;
      continue;
    }
    if (content->IsElement()) {
      if (passedChild) {
        PostRestyleEvent(content->AsElement(), eRestyle_Subtree,
                         nsChangeHint(0));
      }
      break;
    }
  }
}

// Needed since we can't use PostRestyleEvent on non-elements (with
// eRestyle_LaterSiblings or nsRestyleHint(eRestyle_Subtree |
// eRestyle_LaterSiblings) as appropriate).
static void
RestyleSiblingsStartingWith(RestyleManager* aRestyleManager,
                            nsIContent* aStartingSibling /* may be null */)
{
  for (nsIContent* sibling = aStartingSibling; sibling;
       sibling = sibling->GetNextSibling()) {
    if (sibling->IsElement()) {
      aRestyleManager->
        PostRestyleEvent(sibling->AsElement(),
                         nsRestyleHint(eRestyle_Subtree | eRestyle_LaterSiblings),
                         nsChangeHint(0));
      break;
    }
  }
}

template<typename CharT>
bool
WhitespaceOnly(const CharT* aBuffer, size_t aUpTo)
{
  for (auto index : IntegerRange(aUpTo)) {
    if (!dom::IsSpaceCharacter(aBuffer[index])) {
      return false;
    }
  }
  return true;
}

template<typename CharT>
bool
WhitespaceOnlyChangedOnAppend(const CharT* aBuffer,
                              size_t aOldLength,
                              size_t aNewLength)
{
  MOZ_ASSERT(aOldLength <= aNewLength);
  if (!WhitespaceOnly(aBuffer, aOldLength)) {
    // The old text was already not whitespace-only.
    return false;
  }

  return !WhitespaceOnly(aBuffer + aOldLength, aNewLength - aOldLength);
}

static bool
HasAnySignificantSibling(Element* aContainer, nsIContent* aChild)
{
  MOZ_ASSERT(aChild->GetParent() == aContainer);
  for (nsIContent* child = aContainer->GetFirstChild();
       child;
       child = child->GetNextSibling()) {
    if (child == aChild) {
      continue;
    }
    // We don't know whether we're testing :empty or :-moz-only-whitespace,
    // so be conservative and assume :-moz-only-whitespace (i.e., make
    // IsSignificantChild less likely to be true, and thus make us more
    // likely to restyle).
    if (nsStyleUtil::IsSignificantChild(child, false)) {
      return true;
    }
  }

  return false;
}

void
RestyleManager::CharacterDataChanged(nsIContent* aContent,
                                     const CharacterDataChangeInfo& aInfo)
{
  nsINode* parent = aContent->GetParentNode();
  MOZ_ASSERT(parent, "How were we notified of a stray node?");

  uint32_t slowSelectorFlags = parent->GetFlags() & NODE_ALL_SELECTOR_FLAGS;
  if (!(slowSelectorFlags & (NODE_HAS_EMPTY_SELECTOR |
                             NODE_HAS_EDGE_CHILD_SELECTOR))) {
    // Nothing to do, no other slow selector can change as a result of this.
    return;
  }

  if (!aContent->IsNodeOfType(nsINode::eTEXT)) {
    // Doesn't matter to styling (could be a processing instruction or a
    // comment), it can't change whether any selectors match or don't.
    return;
  }


  if (MOZ_UNLIKELY(!parent->IsElement())) {
    MOZ_ASSERT(parent->IsShadowRoot());
    return;
  }

  if (MOZ_UNLIKELY(aContent->IsRootOfAnonymousSubtree())) {
    // This is an anonymous node and thus isn't in child lists, so isn't taken
    // into account for selector matching the relevant selectors here.
    return;
  }

  // Handle appends specially since they're common and we can know both the old
  // and the new text exactly.
  //
  // TODO(emilio): This could be made much more general if :-moz-only-whitespace
  // / :-moz-first-node and :-moz-last-node didn't exist. In that case we only
  // need to know whether we went from empty to non-empty, and that's trivial to
  // know, with CharacterDataChangeInfo...
  if (!aInfo.mAppend) {
    // FIXME(emilio): This restyles unnecessarily if the text node is the only
    // child of the parent element. Fortunately, it's uncommon to have such
    // nodes and this not being an append.
    //
    // See the testcase in bug 1427625 for a test-case that triggers this.
    RestyleForInsertOrChange(aContent);
    return;
  }

  const nsTextFragment* text = aContent->GetText();

  const size_t oldLength = aInfo.mChangeStart;
  const size_t newLength = text->GetLength();

  const bool emptyChanged = !oldLength && newLength;

  const bool whitespaceOnlyChanged = text->Is2b()
    ? WhitespaceOnlyChangedOnAppend(text->Get2b(), oldLength, newLength)
    : WhitespaceOnlyChangedOnAppend(text->Get1b(), oldLength, newLength);

  if (!emptyChanged && !whitespaceOnlyChanged) {
    return;
  }

  if (slowSelectorFlags & NODE_HAS_EMPTY_SELECTOR) {
    if (!HasAnySignificantSibling(parent->AsElement(), aContent)) {
      // We used to be empty, restyle the parent.
      RestyleForEmptyChange(parent->AsElement());
      return;
    }
  }

  if (slowSelectorFlags & NODE_HAS_EDGE_CHILD_SELECTOR) {
    MaybeRestyleForEdgeChildChange(parent->AsElement(), aContent);
  }
}

// Restyling for a ContentInserted or CharacterDataChanged notification.
// This could be used for ContentRemoved as well if we got the
// notification before the removal happened (and sometimes
// CharacterDataChanged is more like a removal than an addition).
// The comments are written and variables are named in terms of it being
// a ContentInserted notification.
void
RestyleManager::RestyleForInsertOrChange(nsIContent* aChild)
{
  nsINode* parentNode = aChild->GetParentNode();

  MOZ_ASSERT(parentNode);
  // The container might be a document or a ShadowRoot.
  if (!parentNode->IsElement()) {
    return;
  }
  Element* container = parentNode->AsElement();

  NS_ASSERTION(!aChild->IsRootOfAnonymousSubtree(),
               "anonymous nodes should not be in child lists");
  uint32_t selectorFlags = container->GetFlags() & NODE_ALL_SELECTOR_FLAGS;
  if (selectorFlags == 0)
    return;

  if (selectorFlags & NODE_HAS_EMPTY_SELECTOR) {
    // See whether we need to restyle the container due to :empty /
    // :-moz-only-whitespace.
    const bool wasEmpty = !HasAnySignificantSibling(container, aChild);
    if (wasEmpty) {
      // FIXME(emilio): When coming from CharacterDataChanged this can restyle
      // unnecessarily. Also can restyle unnecessarily if aChild is not
      // significant anyway, though that's more unlikely.
      RestyleForEmptyChange(container);
      return;
    }
  }

  if (selectorFlags & NODE_HAS_SLOW_SELECTOR) {
    PostRestyleEvent(container, eRestyle_Subtree, nsChangeHint(0));
    // Restyling the container is the most we can do here, so we're done.
    return;
  }

  if (selectorFlags & NODE_HAS_SLOW_SELECTOR_LATER_SIBLINGS) {
    // Restyle all later siblings.
    RestyleSiblingsStartingWith(this, aChild->GetNextSibling());
  }

  if (selectorFlags & NODE_HAS_EDGE_CHILD_SELECTOR) {
    MaybeRestyleForEdgeChildChange(container, aChild);
  }
}

void
RestyleManager::ContentRemoved(nsIContent* aOldChild,
                               nsIContent* aFollowingSibling)
{
  MOZ_ASSERT(aOldChild->GetParentNode());

  // Computed style data isn't useful for detached nodes, and we'll need to
  // recompute it anyway if we ever insert the nodes back into a document.
  if (aOldChild->IsElement()) {
    RestyleManager::ClearServoDataFromSubtree(aOldChild->AsElement());
  }

  // The container might be a document or a ShadowRoot.
  if (!aOldChild->GetParentNode()->IsElement()) {
    return;
  }
  Element* container = aOldChild->GetParentNode()->AsElement();

  if (aOldChild->IsRootOfAnonymousSubtree()) {
    // This should be an assert, but this is called incorrectly in
    // HTMLEditor::DeleteRefToAnonymousNode and the assertions were clogging
    // up the logs.  Make it an assert again when that's fixed.
    MOZ_ASSERT(aOldChild->GetProperty(nsGkAtoms::restylableAnonymousNode),
               "anonymous nodes should not be in child lists (bug 439258)");
  }
  uint32_t selectorFlags = container->GetFlags() & NODE_ALL_SELECTOR_FLAGS;
  if (selectorFlags == 0)
    return;

  if (selectorFlags & NODE_HAS_EMPTY_SELECTOR) {
    // see whether we need to restyle the container
    bool isEmpty = true; // :empty or :-moz-only-whitespace
    for (nsIContent* child = container->GetFirstChild();
         child;
         child = child->GetNextSibling()) {
      // We don't know whether we're testing :empty or :-moz-only-whitespace,
      // so be conservative and assume :-moz-only-whitespace (i.e., make
      // IsSignificantChild less likely to be true, and thus make us more
      // likely to restyle).
      if (nsStyleUtil::IsSignificantChild(child, false)) {
        isEmpty = false;
        break;
      }
    }
    if (isEmpty) {
      RestyleForEmptyChange(container);
      return;
    }
  }

  if (selectorFlags & NODE_HAS_SLOW_SELECTOR) {
    PostRestyleEvent(container, eRestyle_Subtree, nsChangeHint(0));
    // Restyling the container is the most we can do here, so we're done.
    return;
  }

  if (selectorFlags & NODE_HAS_SLOW_SELECTOR_LATER_SIBLINGS) {
    // Restyle all later siblings.
    RestyleSiblingsStartingWith(this, aFollowingSibling);
  }

  if (selectorFlags & NODE_HAS_EDGE_CHILD_SELECTOR) {
    // restyle the now-first element child if it was after aOldChild
    bool reachedFollowingSibling = false;
    for (nsIContent* content = container->GetFirstChild();
         content;
         content = content->GetNextSibling()) {
      if (content == aFollowingSibling) {
        reachedFollowingSibling = true;
        // do NOT continue here; we might want to restyle this node
      }
      if (content->IsElement()) {
        if (reachedFollowingSibling) {
          PostRestyleEvent(content->AsElement(), eRestyle_Subtree,
                           nsChangeHint(0));
        }
        break;
      }
    }
    // restyle the now-last element child if it was before aOldChild
    reachedFollowingSibling = (aFollowingSibling == nullptr);
    for (nsIContent* content = container->GetLastChild();
         content;
         content = content->GetPreviousSibling()) {
      if (content->IsElement()) {
        if (reachedFollowingSibling) {
          PostRestyleEvent(content->AsElement(), eRestyle_Subtree, nsChangeHint(0));
        }
        break;
      }
      if (content == aFollowingSibling) {
        reachedFollowingSibling = true;
      }
    }
  }
}

/**
 * Calculates the change hint and the restyle hint for a given content state
 * change.
 *
 * This is called from both Restyle managers.
 */
void
RestyleManager::ContentStateChangedInternal(Element* aElement,
                                            EventStates aStateMask,
                                            nsChangeHint* aOutChangeHint)
{
  MOZ_ASSERT(!mInStyleRefresh);
  MOZ_ASSERT(aOutChangeHint);

  *aOutChangeHint = nsChangeHint(0);
  // Any change to a content state that affects which frames we construct
  // must lead to a frame reconstruct here if we already have a frame.
  // Note that we never decide through non-CSS means to not create frames
  // based on content states, so if we already don't have a frame we don't
  // need to force a reframe -- if it's needed, the HasStateDependentStyle
  // call will handle things.
  nsIFrame* primaryFrame = aElement->GetPrimaryFrame();
  if (primaryFrame) {
    // If it's generated content, ignore LOADING/etc state changes on it.
    if (!primaryFrame->IsGeneratedContentFrame() &&
        aStateMask.HasAtLeastOneOfStates(NS_EVENT_STATE_BROKEN |
                                         NS_EVENT_STATE_USERDISABLED |
                                         NS_EVENT_STATE_SUPPRESSED |
                                         NS_EVENT_STATE_LOADING)) {
      *aOutChangeHint = nsChangeHint_ReconstructFrame;
    } else {
      uint8_t app = primaryFrame->StyleDisplay()->mAppearance;
      if (app) {
        nsITheme* theme = PresContext()->GetTheme();
        if (theme &&
            theme->ThemeSupportsWidget(PresContext(), primaryFrame, app)) {
          bool repaint = false;
          theme->WidgetStateChanged(primaryFrame, app, nullptr, &repaint,
                                    nullptr);
          if (repaint) {
            *aOutChangeHint |= nsChangeHint_RepaintFrame;
          }
        }
      }
    }

    primaryFrame->ContentStatesChanged(aStateMask);
  }

  if (aStateMask.HasState(NS_EVENT_STATE_VISITED)) {
    // Exposing information to the page about whether the link is
    // visited or not isn't really something we can worry about here.
    // FIXME: We could probably do this a bit better.
    *aOutChangeHint |= nsChangeHint_RepaintFrame;
  }
}

/* static */ nsCString
RestyleManager::RestyleHintToString(nsRestyleHint aHint)
{
  nsCString result;
  bool any = false;
  const char* names[] = {
    "Self", "SomeDescendants", "Subtree", "LaterSiblings", "CSSTransitions",
    "CSSAnimations", "StyleAttribute", "StyleAttribute_Animations",
    "Force", "ForceDescendants"
  };
  uint32_t hint = aHint & ((1 << ArrayLength(names)) - 1);
  uint32_t rest = aHint & ~((1 << ArrayLength(names)) - 1);
  for (uint32_t i = 0; i < ArrayLength(names); i++) {
    if (hint & (1 << i)) {
      if (any) {
        result.AppendLiteral(" | ");
      }
      result.AppendPrintf("eRestyle_%s", names[i]);
      any = true;
    }
  }
  if (rest) {
    if (any) {
      result.AppendLiteral(" | ");
    }
    result.AppendPrintf("0x%0x", rest);
  } else {
    if (!any) {
      result.AppendLiteral("0");
    }
  }
  return result;
}

#ifdef DEBUG
/* static */ nsCString
RestyleManager::ChangeHintToString(nsChangeHint aHint)
{
  nsCString result;
  bool any = false;
  const char* names[] = {
    "RepaintFrame", "NeedReflow", "ClearAncestorIntrinsics",
    "ClearDescendantIntrinsics", "NeedDirtyReflow", "SyncFrameView",
    "UpdateCursor", "UpdateEffects", "UpdateOpacityLayer",
    "UpdateTransformLayer", "ReconstructFrame", "UpdateOverflow",
    "UpdateSubtreeOverflow", "UpdatePostTransformOverflow",
    "UpdateParentOverflow",
    "ChildrenOnlyTransform", "RecomputePosition", "UpdateContainingBlock",
    "BorderStyleNoneChange", "UpdateTextPath", "SchedulePaint",
    "NeutralChange", "InvalidateRenderingObservers",
    "ReflowChangesSizeOrPosition", "UpdateComputedBSize",
    "UpdateUsesOpacity", "UpdateBackgroundPosition",
    "AddOrRemoveTransform", "CSSOverflowChange",
    "UpdateWidgetProperties", "UpdateTableCellSpans",
    "VisibilityChange"
  };
  static_assert(nsChangeHint_AllHints ==
                  static_cast<uint32_t>((1ull << ArrayLength(names)) - 1),
                "Name list doesn't match change hints.");
  uint32_t hint = aHint & static_cast<uint32_t>((1ull << ArrayLength(names)) - 1);
  uint32_t rest = aHint & ~static_cast<uint32_t>((1ull << ArrayLength(names)) - 1);
  if ((hint & NS_STYLE_HINT_REFLOW) == NS_STYLE_HINT_REFLOW) {
    result.AppendLiteral("NS_STYLE_HINT_REFLOW");
    hint = hint & ~NS_STYLE_HINT_REFLOW;
    any = true;
  } else if ((hint & nsChangeHint_AllReflowHints) == nsChangeHint_AllReflowHints) {
    result.AppendLiteral("nsChangeHint_AllReflowHints");
    hint = hint & ~nsChangeHint_AllReflowHints;
    any = true;
  } else if ((hint & NS_STYLE_HINT_VISUAL) == NS_STYLE_HINT_VISUAL) {
    result.AppendLiteral("NS_STYLE_HINT_VISUAL");
    hint = hint & ~NS_STYLE_HINT_VISUAL;
    any = true;
  }
  for (uint32_t i = 0; i < ArrayLength(names); i++) {
    if (hint & (1u << i)) {
      if (any) {
        result.AppendLiteral(" | ");
      }
      result.AppendPrintf("nsChangeHint_%s", names[i]);
      any = true;
    }
  }
  if (rest) {
    if (any) {
      result.AppendLiteral(" | ");
    }
    result.AppendPrintf("0x%0x", rest);
  } else {
    if (!any) {
      result.AppendLiteral("nsChangeHint(0)");
    }
  }
  return result;
}
#endif

/**
 * Frame construction helpers follow.
 */
#ifdef DEBUG
static bool gInApplyRenderingChangeToTree = false;
#endif

/**
 * Sync views on aFrame and all of aFrame's descendants (following placeholders),
 * if aChange has nsChangeHint_SyncFrameView.
 * Calls DoApplyRenderingChangeToTree on all aFrame's out-of-flow descendants
 * (following placeholders), if aChange has nsChangeHint_RepaintFrame.
 * aFrame should be some combination of nsChangeHint_SyncFrameView,
 * nsChangeHint_RepaintFrame, nsChangeHint_UpdateOpacityLayer and
 * nsChangeHint_SchedulePaint, nothing else.
*/
static void SyncViewsAndInvalidateDescendants(nsIFrame* aFrame,
                                              nsChangeHint aChange);

static void StyleChangeReflow(nsIFrame* aFrame, nsChangeHint aHint);

/**
 * To handle nsChangeHint_ChildrenOnlyTransform we must iterate over the child
 * frames of the SVG frame concerned. This helper function is used to find that
 * SVG frame when we encounter nsChangeHint_ChildrenOnlyTransform to ensure
 * that we iterate over the intended children, since sometimes we end up
 * handling that hint while processing hints for one of the SVG frame's
 * ancestor frames.
 *
 * The reason that we sometimes end up trying to process the hint for an
 * ancestor of the SVG frame that the hint is intended for is due to the way we
 * process restyle events. ApplyRenderingChangeToTree adjusts the frame from
 * the restyled element's principle frame to one of its ancestor frames based
 * on what nsCSSRendering::FindBackground returns, since the background style
 * may have been propagated up to an ancestor frame. Processing hints using an
 * ancestor frame is fine in general, but nsChangeHint_ChildrenOnlyTransform is
 * a special case since it is intended to update the children of a specific
 * frame.
 */
static nsIFrame*
GetFrameForChildrenOnlyTransformHint(nsIFrame* aFrame)
{
  if (aFrame->IsViewportFrame()) {
    // This happens if the root-<svg> is fixed positioned, in which case we
    // can't use aFrame->GetContent() to find the primary frame, since
    // GetContent() returns nullptr for ViewportFrame.
    aFrame = aFrame->PrincipalChildList().FirstChild();
  }
  // For an nsHTMLScrollFrame, this will get the SVG frame that has the
  // children-only transforms:
  aFrame = aFrame->GetContent()->GetPrimaryFrame();
  if (aFrame->IsSVGOuterSVGFrame()) {
    aFrame = aFrame->PrincipalChildList().FirstChild();
    MOZ_ASSERT(aFrame->IsSVGOuterSVGAnonChildFrame(),
               "Where is the nsSVGOuterSVGFrame's anon child??");
  }
  MOZ_ASSERT(aFrame->IsFrameOfType(nsIFrame::eSVG | nsIFrame::eSVGContainer),
             "Children-only transforms only expected on SVG frames");
  return aFrame;
}

// Returns true if this function managed to successfully move a frame, and
// false if it could not process the position change, and a reflow should
// be performed instead.
static bool
RecomputePosition(nsIFrame* aFrame)
{
  // Don't process position changes on table frames, since we already handle
  // the dynamic position change on the table wrapper frame, and the
  // reflow-based fallback code path also ignores positions on inner table
  // frames.
  if (aFrame->IsTableFrame()) {
    return true;
  }

  const nsStyleDisplay* display = aFrame->StyleDisplay();
  // Changes to the offsets of a non-positioned element can safely be ignored.
  if (display->mPosition == NS_STYLE_POSITION_STATIC) {
    return true;
  }

  // Don't process position changes on frames which have views or the ones which
  // have a view somewhere in their descendants, because the corresponding view
  // needs to be repositioned properly as well.
  if (aFrame->HasView() ||
      (aFrame->GetStateBits() & NS_FRAME_HAS_CHILD_WITH_VIEW)) {
    StyleChangeReflow(aFrame, nsChangeHint_NeedReflow);
    return false;
  }

  // Flexbox and Grid layout supports CSS Align and the optimizations below
  // don't support that yet.
  if (aFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW)) {
    nsIFrame* ph = aFrame->GetPlaceholderFrame();
    if (ph && ph->HasAnyStateBits(PLACEHOLDER_STATICPOS_NEEDS_CSSALIGN)) {
      StyleChangeReflow(aFrame, nsChangeHint_NeedReflow);
      return false;
    }
  }

  aFrame->SchedulePaint();

  // For relative positioning, we can simply update the frame rect
  if (display->IsRelativelyPositionedStyle()) {
    // Move the frame
    if (display->mPosition == NS_STYLE_POSITION_STICKY) {
      // Update sticky positioning for an entire element at once, starting with
      // the first continuation or ib-split sibling.
      // It's rare that the frame we already have isn't already the first
      // continuation or ib-split sibling, but it can happen when styles differ
      // across continuations such as ::first-line or ::first-letter, and in
      // those cases we will generally (but maybe not always) do the work twice.
      nsIFrame* firstContinuation =
        nsLayoutUtils::FirstContinuationOrIBSplitSibling(aFrame);

      StickyScrollContainer::ComputeStickyOffsets(firstContinuation);
      StickyScrollContainer* ssc =
        StickyScrollContainer::GetStickyScrollContainerForFrame(
          firstContinuation);
      if (ssc) {
        ssc->PositionContinuations(firstContinuation);
      }
    } else {
      MOZ_ASSERT(NS_STYLE_POSITION_RELATIVE == display->mPosition,
                 "Unexpected type of positioning");
      for (nsIFrame* cont = aFrame; cont;
           cont = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
        nsIFrame* cb = cont->GetContainingBlock();
        nsMargin newOffsets;
        WritingMode wm = cb->GetWritingMode();
        const LogicalSize size(wm, cb->GetContentRectRelativeToSelf().Size());

        ReflowInput::ComputeRelativeOffsets(wm, cont, size, newOffsets);
        NS_ASSERTION(newOffsets.left == -newOffsets.right &&
                     newOffsets.top == -newOffsets.bottom,
                     "ComputeRelativeOffsets should return valid results");

        // ReflowInput::ApplyRelativePositioning would work here, but
        // since we've already checked mPosition and aren't changing the frame's
        // normal position, go ahead and add the offsets directly.
        // First, we need to ensure that the normal position is stored though.
        bool hasProperty;
        nsPoint normalPosition = cont->GetNormalPosition(&hasProperty);
        if (!hasProperty) {
          cont->AddProperty(nsIFrame::NormalPositionProperty(),
                            new nsPoint(normalPosition));
        }
        cont->SetPosition(normalPosition +
                          nsPoint(newOffsets.left, newOffsets.top));
      }
    }

    return true;
  }

  // For the absolute positioning case, set up a fake HTML reflow state for
  // the frame, and then get the offsets and size from it. If the frame's size
  // doesn't need to change, we can simply update the frame position. Otherwise
  // we fall back to a reflow.
  RefPtr<gfxContext> rc =
    aFrame->PresShell()->CreateReferenceRenderingContext();

  // Construct a bogus parent reflow state so that there's a usable
  // containing block reflow state.
  nsIFrame* parentFrame = aFrame->GetParent();
  WritingMode parentWM = parentFrame->GetWritingMode();
  WritingMode frameWM = aFrame->GetWritingMode();
  LogicalSize parentSize = parentFrame->GetLogicalSize();

  nsFrameState savedState = parentFrame->GetStateBits();
  ReflowInput parentReflowInput(aFrame->PresContext(), parentFrame, rc,
                                parentSize);
  parentFrame->RemoveStateBits(~nsFrameState(0));
  parentFrame->AddStateBits(savedState);

  // The bogus parent state here was created with no parent state of its own,
  // and therefore it won't have an mCBReflowInput set up.
  // But we may need one (for InitCBReflowInput in a child state), so let's
  // try to create one here for the cases where it will be needed.
  Maybe<ReflowInput> cbReflowInput;
  nsIFrame* cbFrame = parentFrame->GetContainingBlock();
  if (cbFrame && (aFrame->GetContainingBlock() != parentFrame ||
                  parentFrame->IsTableFrame())) {
    LogicalSize cbSize = cbFrame->GetLogicalSize();
    cbReflowInput.emplace(cbFrame->PresContext(), cbFrame, rc, cbSize);
    cbReflowInput->ComputedPhysicalMargin() = cbFrame->GetUsedMargin();
    cbReflowInput->ComputedPhysicalPadding() = cbFrame->GetUsedPadding();
    cbReflowInput->ComputedPhysicalBorderPadding() =
      cbFrame->GetUsedBorderAndPadding();
    parentReflowInput.mCBReflowInput = cbReflowInput.ptr();
  }

  NS_WARNING_ASSERTION(parentSize.ISize(parentWM) != NS_INTRINSICSIZE &&
                       parentSize.BSize(parentWM) != NS_INTRINSICSIZE,
                       "parentSize should be valid");
  parentReflowInput.SetComputedISize(std::max(parentSize.ISize(parentWM), 0));
  parentReflowInput.SetComputedBSize(std::max(parentSize.BSize(parentWM), 0));
  parentReflowInput.ComputedPhysicalMargin().SizeTo(0, 0, 0, 0);

  parentReflowInput.ComputedPhysicalPadding() = parentFrame->GetUsedPadding();
  parentReflowInput.ComputedPhysicalBorderPadding() =
    parentFrame->GetUsedBorderAndPadding();
  LogicalSize availSize = parentSize.ConvertTo(frameWM, parentWM);
  availSize.BSize(frameWM) = NS_INTRINSICSIZE;

  ViewportFrame* viewport = do_QueryFrame(parentFrame);
  nsSize cbSize = viewport ?
    viewport->AdjustReflowInputAsContainingBlock(&parentReflowInput).Size()
    : aFrame->GetContainingBlock()->GetSize();
  const nsMargin& parentBorder =
    parentReflowInput.mStyleBorder->GetComputedBorder();
  cbSize -= nsSize(parentBorder.LeftRight(), parentBorder.TopBottom());
  LogicalSize lcbSize(frameWM, cbSize);
  ReflowInput reflowInput(aFrame->PresContext(), parentReflowInput, aFrame,
                          availSize, &lcbSize);
  nsSize computedSize(reflowInput.ComputedWidth(),
                      reflowInput.ComputedHeight());
  computedSize.width += reflowInput.ComputedPhysicalBorderPadding().LeftRight();
  if (computedSize.height != NS_INTRINSICSIZE) {
    computedSize.height +=
      reflowInput.ComputedPhysicalBorderPadding().TopBottom();
  }
  nsSize size = aFrame->GetSize();
  // The RecomputePosition hint is not used if any offset changed between auto
  // and non-auto. If computedSize.height == NS_INTRINSICSIZE then the new
  // element height will be its intrinsic height, and since 'top' and 'bottom''s
  // auto-ness hasn't changed, the old height must also be its intrinsic
  // height, which we can assume hasn't changed (or reflow would have
  // been triggered).
  if (computedSize.width == size.width &&
      (computedSize.height == NS_INTRINSICSIZE || computedSize.height == size.height)) {
    // If we're solving for 'left' or 'top', then compute it here, in order to
    // match the reflow code path.
    if (NS_AUTOOFFSET == reflowInput.ComputedPhysicalOffsets().left) {
      reflowInput.ComputedPhysicalOffsets().left = cbSize.width -
                                          reflowInput.ComputedPhysicalOffsets().right -
                                          reflowInput.ComputedPhysicalMargin().right -
                                          size.width -
                                          reflowInput.ComputedPhysicalMargin().left;
    }

    if (NS_AUTOOFFSET == reflowInput.ComputedPhysicalOffsets().top) {
      reflowInput.ComputedPhysicalOffsets().top = cbSize.height -
                                         reflowInput.ComputedPhysicalOffsets().bottom -
                                         reflowInput.ComputedPhysicalMargin().bottom -
                                         size.height -
                                         reflowInput.ComputedPhysicalMargin().top;
    }

    // Move the frame
    nsPoint pos(parentBorder.left + reflowInput.ComputedPhysicalOffsets().left +
                reflowInput.ComputedPhysicalMargin().left,
                parentBorder.top + reflowInput.ComputedPhysicalOffsets().top +
                reflowInput.ComputedPhysicalMargin().top);
    aFrame->SetPosition(pos);

    return true;
  }

  // Fall back to a reflow
  StyleChangeReflow(aFrame, nsChangeHint_NeedReflow);
  return false;
}

static bool
HasBoxAncestor(nsIFrame* aFrame)
{
  for (nsIFrame* f = aFrame; f; f = f->GetParent()) {
    if (f->IsXULBoxFrame()) {
      return true;
    }
  }
  return false;
}

/**
 * Return true if aFrame's subtree has placeholders for out-of-flow content
 * whose 'position' style's bit in aPositionMask is set.
 */
static bool
FrameHasPositionedPlaceholderDescendants(nsIFrame* aFrame,
                                         uint32_t aPositionMask)
{
  MOZ_ASSERT(aPositionMask & (1 << NS_STYLE_POSITION_FIXED));

  for (nsIFrame::ChildListIterator lists(aFrame); !lists.IsDone(); lists.Next()) {
    for (nsIFrame* f : lists.CurrentList()) {
      if (f->IsPlaceholderFrame()) {
        nsIFrame* outOfFlow =
          nsPlaceholderFrame::GetRealFrameForPlaceholder(f);
        // If SVG text frames could appear here, they could confuse us since
        // they ignore their position style ... but they can't.
        NS_ASSERTION(!nsSVGUtils::IsInSVGTextSubtree(outOfFlow),
                     "SVG text frames can't be out of flow");
        if (aPositionMask & (1 << outOfFlow->StyleDisplay()->mPosition)) {
          return true;
        }
      }
      uint32_t positionMask = aPositionMask;
      // NOTE:  It's tempting to check f->IsAbsPosContainingBlock() or
      // f->IsFixedPosContainingBlock() here.  However, that would only
      // be testing the *new* style of the frame, which might exclude
      // descendants that currently have this frame as an abs-pos
      // containing block.  Taking the codepath where we don't reframe
      // could lead to an unsafe call to
      // cont->MarkAsNotAbsoluteContainingBlock() before we've reframed
      // the descendant and taken it off the absolute list.
      if (FrameHasPositionedPlaceholderDescendants(f, positionMask)) {
        return true;
      }
    }
  }
  return false;
}

static bool
NeedToReframeForAddingOrRemovingTransform(nsIFrame* aFrame)
{
  static_assert(0 <= NS_STYLE_POSITION_ABSOLUTE &&
                NS_STYLE_POSITION_ABSOLUTE < 32, "Style constant out of range");
  static_assert(0 <= NS_STYLE_POSITION_FIXED &&
                NS_STYLE_POSITION_FIXED < 32, "Style constant out of range");

  uint32_t positionMask;
  // Don't call aFrame->IsPositioned here, since that returns true if
  // the frame already has a transform, and we want to ignore that here
  if (aFrame->IsAbsolutelyPositioned() || aFrame->IsRelativelyPositioned()) {
    // This frame is a container for abs-pos descendants whether or not it
    // has a transform.
    // So abs-pos descendants are no problem; we only need to reframe if
    // we have fixed-pos descendants.
    positionMask = 1 << NS_STYLE_POSITION_FIXED;
  } else {
    // This frame may not be a container for abs-pos descendants already.
    // So reframe if we have abs-pos or fixed-pos descendants.
    positionMask =
      (1 << NS_STYLE_POSITION_FIXED) | (1 << NS_STYLE_POSITION_ABSOLUTE);
  }
  for (nsIFrame* f = aFrame; f;
       f = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(f)) {
    if (FrameHasPositionedPlaceholderDescendants(f, positionMask)) {
      return true;
    }
  }
  return false;
}

static void
DoApplyRenderingChangeToTree(nsIFrame* aFrame,
                             nsChangeHint aChange)
{
  NS_PRECONDITION(gInApplyRenderingChangeToTree,
                  "should only be called within ApplyRenderingChangeToTree");

  for ( ; aFrame; aFrame = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(aFrame)) {
    // Invalidate and sync views on all descendant frames, following placeholders.
    // We don't need to update transforms in SyncViewsAndInvalidateDescendants, because
    // there can't be any out-of-flows or popups that need to be transformed;
    // all out-of-flow descendants of the transformed element must also be
    // descendants of the transformed frame.
    SyncViewsAndInvalidateDescendants(aFrame,
      nsChangeHint(aChange & (nsChangeHint_RepaintFrame |
                              nsChangeHint_SyncFrameView |
                              nsChangeHint_UpdateOpacityLayer |
                              nsChangeHint_SchedulePaint)));
    // This must be set to true if the rendering change needs to
    // invalidate content.  If it's false, a composite-only paint
    // (empty transaction) will be scheduled.
    bool needInvalidatingPaint = false;

    // if frame has view, will already be invalidated
    if (aChange & nsChangeHint_RepaintFrame) {
      // Note that this whole block will be skipped when painting is suppressed
      // (due to our caller ApplyRendingChangeToTree() discarding the
      // nsChangeHint_RepaintFrame hint).  If you add handling for any other
      // hints within this block, be sure that they too should be ignored when
      // painting is suppressed.
      needInvalidatingPaint = true;
      aFrame->InvalidateFrameSubtree();
      if ((aChange & nsChangeHint_UpdateEffects) &&
          aFrame->IsFrameOfType(nsIFrame::eSVG) &&
          !(aFrame->GetStateBits() & NS_STATE_IS_OUTER_SVG)) {
        // Need to update our overflow rects:
        nsSVGUtils::ScheduleReflowSVG(aFrame);
      }
    }
    if (aChange & nsChangeHint_UpdateTextPath) {
      if (nsSVGUtils::IsInSVGTextSubtree(aFrame)) {
        // Invalidate and reflow the entire SVGTextFrame:
        NS_ASSERTION(aFrame->GetContent()->IsSVGElement(nsGkAtoms::textPath),
                     "expected frame for a <textPath> element");
        nsIFrame* text = nsLayoutUtils::GetClosestFrameOfType(
          aFrame, LayoutFrameType::SVGText);
        NS_ASSERTION(text, "expected to find an ancestor SVGTextFrame");
        static_cast<SVGTextFrame*>(text)->NotifyGlyphMetricsChange();
      } else {
        MOZ_ASSERT(false, "unexpected frame got nsChangeHint_UpdateTextPath");
      }
    }
    if (aChange & nsChangeHint_UpdateOpacityLayer) {
      // FIXME/bug 796697: we can get away with empty transactions for
      // opacity updates in many cases.
      needInvalidatingPaint = true;

      ActiveLayerTracker::NotifyRestyle(aFrame, eCSSProperty_opacity);
      if (nsSVGIntegrationUtils::UsingEffectsForFrame(aFrame)) {
        // SVG effects paints the opacity without using
        // nsDisplayOpacity. We need to invalidate manually.
        aFrame->InvalidateFrameSubtree();
      }
    }
    if ((aChange & nsChangeHint_UpdateTransformLayer) &&
        aFrame->IsTransformed()) {
      ActiveLayerTracker::NotifyRestyle(aFrame, eCSSProperty_transform);
      // If we're not already going to do an invalidating paint, see
      // if we can get away with only updating the transform on a
      // layer for this frame, and not scheduling an invalidating
      // paint.
      if (!needInvalidatingPaint) {
        nsDisplayItem::Layer* layer;
        needInvalidatingPaint |= !aFrame->TryUpdateTransformOnly(&layer);

        if (!needInvalidatingPaint) {
          // Since we're not going to paint, we need to resend animation
          // data to the layer.
          MOZ_ASSERT(layer, "this can't happen if there's no layer");
          nsDisplayListBuilder::AddAnimationsAndTransitionsToLayer(
            layer, nullptr, nullptr, aFrame, eCSSProperty_transform);
        }
      }
    }
    if (aChange & nsChangeHint_ChildrenOnlyTransform) {
      needInvalidatingPaint = true;
      nsIFrame* childFrame =
        GetFrameForChildrenOnlyTransformHint(aFrame)->PrincipalChildList().FirstChild();
      for ( ; childFrame; childFrame = childFrame->GetNextSibling()) {
        ActiveLayerTracker::NotifyRestyle(childFrame, eCSSProperty_transform);
      }
    }
    if (aChange & nsChangeHint_SchedulePaint) {
      needInvalidatingPaint = true;
    }
    aFrame->SchedulePaint(needInvalidatingPaint
                            ? nsIFrame::PAINT_DEFAULT
                            : nsIFrame::PAINT_COMPOSITE_ONLY);
  }
}

static void
SyncViewsAndInvalidateDescendants(nsIFrame* aFrame, nsChangeHint aChange)
{
  NS_PRECONDITION(gInApplyRenderingChangeToTree,
                  "should only be called within ApplyRenderingChangeToTree");
  NS_ASSERTION(nsChangeHint_size_t(aChange) ==
                          (aChange & (nsChangeHint_RepaintFrame |
                                      nsChangeHint_SyncFrameView |
                                      nsChangeHint_UpdateOpacityLayer |
                                      nsChangeHint_SchedulePaint)),
               "Invalid change flag");

  if (aChange & nsChangeHint_SyncFrameView) {
    aFrame->SyncFrameViewProperties();
  }

  nsIFrame::ChildListIterator lists(aFrame);
  for (; !lists.IsDone(); lists.Next()) {
    for (nsIFrame* child : lists.CurrentList()) {
      if (!(child->GetStateBits() & NS_FRAME_OUT_OF_FLOW)) {
        // only do frames that don't have placeholders
        if (child->IsPlaceholderFrame()) {
          // do the out-of-flow frame and its continuations
          nsIFrame* outOfFlowFrame =
            nsPlaceholderFrame::GetRealFrameForPlaceholder(child);
          DoApplyRenderingChangeToTree(outOfFlowFrame, aChange);
        } else if (lists.CurrentID() == nsIFrame::kPopupList) {
          DoApplyRenderingChangeToTree(child, aChange);
        } else { // regular frame
          SyncViewsAndInvalidateDescendants(child, aChange);
        }
      }
    }
  }
}

static void
ApplyRenderingChangeToTree(nsIPresShell* aPresShell,
                           nsIFrame* aFrame,
                           nsChangeHint aChange)
{
  // We check StyleDisplay()->HasTransformStyle() in addition to checking
  // IsTransformed() since we can get here for some frames that don't support
  // CSS transforms.
  NS_ASSERTION(!(aChange & nsChangeHint_UpdateTransformLayer) ||
               aFrame->IsTransformed() ||
               aFrame->StyleDisplay()->HasTransformStyle(),
               "Unexpected UpdateTransformLayer hint");

  if (aPresShell->IsPaintingSuppressed()) {
    // Don't allow synchronous rendering changes when painting is turned off.
    aChange &= ~nsChangeHint_RepaintFrame;
    if (!aChange) {
      return;
    }
  }

// Trigger rendering updates by damaging this frame and any
// continuations of this frame.
#ifdef DEBUG
  gInApplyRenderingChangeToTree = true;
#endif
  if (aChange & nsChangeHint_RepaintFrame) {
    // If the frame's background is propagated to an ancestor, walk up to
    // that ancestor and apply the RepaintFrame change hint to it.
    ComputedStyle* bgSC;
    nsIFrame* propagatedFrame = aFrame;
    while (!nsCSSRendering::FindBackground(propagatedFrame, &bgSC)) {
      propagatedFrame = propagatedFrame->GetParent();
      NS_ASSERTION(aFrame, "root frame must paint");
    }

    if (propagatedFrame != aFrame) {
      DoApplyRenderingChangeToTree(propagatedFrame, nsChangeHint_RepaintFrame);
      aChange &= ~nsChangeHint_RepaintFrame;
      if (!aChange) {
        return;
      }
    }
  }
  DoApplyRenderingChangeToTree(aFrame, aChange);
#ifdef DEBUG
  gInApplyRenderingChangeToTree = false;
#endif
}

static void
AddSubtreeToOverflowTracker(nsIFrame* aFrame,
                            OverflowChangedTracker& aOverflowChangedTracker)
{
  if (aFrame->FrameMaintainsOverflow()) {
    aOverflowChangedTracker.AddFrame(aFrame,
                                     OverflowChangedTracker::CHILDREN_CHANGED);
  }
  nsIFrame::ChildListIterator lists(aFrame);
  for (; !lists.IsDone(); lists.Next()) {
    for (nsIFrame* child : lists.CurrentList()) {
      AddSubtreeToOverflowTracker(child, aOverflowChangedTracker);
    }
  }
}

static void
StyleChangeReflow(nsIFrame* aFrame, nsChangeHint aHint)
{
  nsIPresShell::IntrinsicDirty dirtyType;
  if (aHint & nsChangeHint_ClearDescendantIntrinsics) {
    NS_ASSERTION(aHint & nsChangeHint_ClearAncestorIntrinsics,
                 "Please read the comments in nsChangeHint.h");
    NS_ASSERTION(aHint & nsChangeHint_NeedDirtyReflow,
                 "ClearDescendantIntrinsics requires NeedDirtyReflow");
    dirtyType = nsIPresShell::eStyleChange;
  } else if ((aHint & nsChangeHint_UpdateComputedBSize) &&
             aFrame->HasAnyStateBits(
               NS_FRAME_DESCENDANT_INTRINSIC_ISIZE_DEPENDS_ON_BSIZE)) {
    dirtyType = nsIPresShell::eStyleChange;
  } else if (aHint & nsChangeHint_ClearAncestorIntrinsics) {
    dirtyType = nsIPresShell::eTreeChange;
  } else if ((aHint & nsChangeHint_UpdateComputedBSize) &&
             HasBoxAncestor(aFrame)) {
    // The frame's computed BSize is changing, and we have a box ancestor
    // whose cached intrinsic height may need to be updated.
    dirtyType = nsIPresShell::eTreeChange;
  } else {
    dirtyType = nsIPresShell::eResize;
  }

  nsFrameState dirtyBits;
  if (aFrame->GetStateBits() & NS_FRAME_FIRST_REFLOW) {
    dirtyBits = nsFrameState(0);
  } else if ((aHint & nsChangeHint_NeedDirtyReflow) ||
             dirtyType == nsIPresShell::eStyleChange) {
    dirtyBits = NS_FRAME_IS_DIRTY;
  } else {
    dirtyBits = NS_FRAME_HAS_DIRTY_CHILDREN;
  }

  // If we're not going to clear any intrinsic sizes on the frames, and
  // there are no dirty bits to set, then there's nothing to do.
  if (dirtyType == nsIPresShell::eResize && !dirtyBits)
    return;

  nsIPresShell::ReflowRootHandling rootHandling;
  if (aHint & nsChangeHint_ReflowChangesSizeOrPosition) {
    rootHandling = nsIPresShell::ePositionOrSizeChange;
  } else {
    rootHandling = nsIPresShell::eNoPositionOrSizeChange;
  }

  do {
    aFrame->PresShell()->FrameNeedsReflow(
      aFrame, dirtyType, dirtyBits, rootHandling);
    aFrame = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(aFrame);
  } while (aFrame);
}

// Get the next sibling which might have a frame.  This only considers siblings
// that stylo post-traversal looks at, so only elements and text.  In
// particular, it ignores comments.
static nsIContent*
NextSiblingWhichMayHaveFrame(nsIContent* aContent)
{
  for (nsIContent* next = aContent->GetNextSibling();
       next;
       next = next->GetNextSibling()) {
    if (next->IsElement() || next->IsNodeOfType(nsINode::eTEXT)) {
      return next;
    }
  }

  return nullptr;
}

void
RestyleManager::ProcessRestyledFrames(nsStyleChangeList& aChangeList)
{
  NS_ASSERTION(!nsContentUtils::IsSafeToRunScript(),
               "Someone forgot a script blocker");

  // See bug 1378219 comment 9:
  // Recursive calls here are a bit worrying, but apparently do happen in the
  // wild (although not currently in any of our automated tests). Try to get a
  // stack from Nightly/Dev channel to figure out what's going on and whether
  // it's OK.
  MOZ_DIAGNOSTIC_ASSERT(!mDestroyedFrames, "ProcessRestyledFrames recursion");

  if (aChangeList.IsEmpty()) {
    return;
  }

  // If mDestroyedFrames is null, we want to create a new hashtable here
  // and destroy it on exit; but if it is already non-null (because we're in
  // a recursive call), we will continue to use the existing table to
  // accumulate destroyed frames, and NOT clear mDestroyedFrames on exit.
  // We use a MaybeClearDestroyedFrames helper to conditionally reset the
  // mDestroyedFrames pointer when this method returns.
  typedef decltype(mDestroyedFrames) DestroyedFramesT;
  class MOZ_RAII MaybeClearDestroyedFrames
  {
  private:
    DestroyedFramesT& mDestroyedFramesRef; // ref to caller's mDestroyedFrames
    const bool        mResetOnDestruction;
  public:
    explicit MaybeClearDestroyedFrames(DestroyedFramesT& aTarget)
      : mDestroyedFramesRef(aTarget)
      , mResetOnDestruction(!aTarget) // reset only if target starts out null
    {
    }
    ~MaybeClearDestroyedFrames()
    {
      if (mResetOnDestruction) {
        mDestroyedFramesRef.reset(nullptr);
      }
    }
  };

  MaybeClearDestroyedFrames maybeClear(mDestroyedFrames);
  if (!mDestroyedFrames) {
    mDestroyedFrames = MakeUnique<nsTHashtable<nsPtrHashKey<const nsIFrame>>>();
  }

  AUTO_PROFILER_LABEL("RestyleManager::ProcessRestyledFrames", CSS);

  nsPresContext* presContext = PresContext();
  nsCSSFrameConstructor* frameConstructor = presContext->FrameConstructor();

  // Handle nsChangeHint_CSSOverflowChange, by either updating the
  // scrollbars on the viewport, or upgrading the change hint to frame-reconstruct.
  for (nsStyleChangeData& data : aChangeList) {
    if (data.mHint & nsChangeHint_CSSOverflowChange) {
      data.mHint &= ~nsChangeHint_CSSOverflowChange;
      bool doReconstruct = true; // assume the worst

      // Only bother with this if we're html/body, since:
      //  (a) It'd be *expensive* to reframe these particular nodes.  They're
      //      at the root, so reframing would mean rebuilding the world.
      //  (b) It's often *unnecessary* to reframe for "overflow" changes on
      //      these particular nodes.  In general, the only reason we reframe
      //      for "overflow" changes is so we can construct (or destroy) a
      //      scrollframe & scrollbars -- and the html/body nodes often don't
      //      need their own scrollframe/scrollbars because they coopt the ones
      //      on the viewport (which always exist). So depending on whether
      //      that's happening, we can skip the reframe for these nodes.
      if (data.mContent->IsAnyOfHTMLElements(nsGkAtoms::body,
                                             nsGkAtoms::html)) {
        // If the restyled element provided/provides the scrollbar styles for
        // the viewport before and/or after this restyle, AND it's not coopting
        // that responsibility from some other element (which would need
        // reconstruction to make its own scrollframe now), THEN: we don't need
        // to reconstruct - we can just reflow, because no scrollframe is being
        // added/removed.
        nsIContent* prevOverrideNode =
          presContext->GetViewportScrollbarStylesOverrideElement();
        nsIContent* newOverrideNode =
          presContext->UpdateViewportScrollbarStylesOverride();

        if (data.mContent == prevOverrideNode ||
            data.mContent == newOverrideNode) {
          // If we get here, the restyled element provided the scrollbar styles
          // for viewport before this restyle, OR it will provide them after.
          if (!prevOverrideNode || !newOverrideNode ||
              prevOverrideNode == newOverrideNode) {
            // If we get here, the restyled element is NOT replacing (or being
            // replaced by) some other element as the viewport's
            // scrollbar-styles provider. (If it were, we'd potentially need to
            // reframe to create a dedicated scrollframe for whichever element
            // is being booted from providing viewport scrollbar styles.)
            //
            // Under these conditions, we're OK to assume that this "overflow"
            // change only impacts the root viewport's scrollframe, which
            // already exists, so we can simply reflow instead of reframing.
            // When requesting this reflow, we send the exact same change hints
            // that "width" and "height" would send (since conceptually,
            // adding/removing scrollbars is like changing the available
            // space).
            data.mHint |= (nsChangeHint_ReflowHintsForISizeChange |
                           nsChangeHint_ReflowHintsForBSizeChange);
            doReconstruct = false;
          }
        }
      }
      if (doReconstruct) {
        data.mHint |= nsChangeHint_ReconstructFrame;
      }
    }
  }

  bool didUpdateCursor = false;

  for (size_t i = 0; i < aChangeList.Length(); ++i) {

    // Collect and coalesce adjacent siblings for lazy frame construction.
    // Eventually it would be even better to make RecreateFramesForContent
    // accept a range and coalesce all adjacent reconstructs (bug 1344139).
    size_t lazyRangeStart = i;
    while (i < aChangeList.Length() &&
           aChangeList[i].mContent &&
           aChangeList[i].mContent->HasFlag(NODE_NEEDS_FRAME) &&
           (i == lazyRangeStart ||
            NextSiblingWhichMayHaveFrame(aChangeList[i - 1].mContent) ==
              aChangeList[i].mContent))
    {
      MOZ_ASSERT(aChangeList[i].mHint & nsChangeHint_ReconstructFrame);
      MOZ_ASSERT(!aChangeList[i].mFrame);
      ++i;
    }
    if (i != lazyRangeStart) {
      nsIContent* start = aChangeList[lazyRangeStart].mContent;
      nsIContent* end = NextSiblingWhichMayHaveFrame(aChangeList[i-1].mContent);
      if (!end) {
        frameConstructor->ContentAppended(
            start,
            nsCSSFrameConstructor::InsertionKind::Sync);
      } else {
        frameConstructor->ContentRangeInserted(
            start,
            end,
            nullptr,
            nsCSSFrameConstructor::InsertionKind::Sync);
      }
    }
    for (size_t j = lazyRangeStart; j < i; ++j) {
      MOZ_ASSERT(!aChangeList[j].mContent->GetPrimaryFrame() ||
                 !aChangeList[j].mContent->HasFlag(NODE_NEEDS_FRAME));
    }
    if (i == aChangeList.Length()) {
      break;
    }

    const nsStyleChangeData& data = aChangeList[i];
    nsIFrame* frame = data.mFrame;
    nsIContent* content = data.mContent;
    nsChangeHint hint = data.mHint;
    bool didReflowThisFrame = false;

    NS_ASSERTION(!(hint & nsChangeHint_AllReflowHints) ||
                 (hint & nsChangeHint_NeedReflow),
                 "Reflow hint bits set without actually asking for a reflow");

    // skip any frame that has been destroyed due to a ripple effect
    if (frame && mDestroyedFrames->Contains(frame)) {
      continue;
    }

    if (frame && frame->GetContent() != content) {
      // XXXbz this is due to image maps messing with the primary frame of
      // <area>s.  See bug 135040.  Remove this block once that's fixed.
      frame = nullptr;
      if (!(hint & nsChangeHint_ReconstructFrame)) {
        continue;
      }
    }

    if ((hint & nsChangeHint_UpdateContainingBlock) && frame &&
        !(hint & nsChangeHint_ReconstructFrame)) {
      if (NeedToReframeForAddingOrRemovingTransform(frame) ||
          frame->IsFieldSetFrame() ||
          frame->GetContentInsertionFrame() != frame) {
        // The frame has positioned children that need to be reparented, or
        // it can't easily be converted to/from being an abs-pos container
        // correctly.
        hint |= nsChangeHint_ReconstructFrame;
      } else {
        for (nsIFrame* cont = frame; cont;
             cont = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
          // Normally frame construction would set state bits as needed,
          // but we're not going to reconstruct the frame so we need to set them.
          // It's because we need to set this state on each affected frame
          // that we can't coalesce nsChangeHint_UpdateContainingBlock hints up
          // to ancestors (i.e. it can't be an change hint that is handled for
          // descendants).
          if (cont->IsAbsPosContainingBlock()) {
            if (!cont->IsAbsoluteContainer() &&
                (cont->GetStateBits() & NS_FRAME_CAN_HAVE_ABSPOS_CHILDREN)) {
              cont->MarkAsAbsoluteContainingBlock();
            }
          } else {
            if (cont->IsAbsoluteContainer()) {
              if (cont->HasAbsolutelyPositionedChildren()) {
                // If |cont| still has absolutely positioned children,
                // we can't call MarkAsNotAbsoluteContainingBlock.  This
                // will remove a frame list that still has children in
                // it that we need to keep track of.
                // The optimization of removing it isn't particularly
                // important, although it does mean we skip some tests.
                NS_WARNING("skipping removal of absolute containing block");
              } else {
                cont->MarkAsNotAbsoluteContainingBlock();
              }
            }
          }
        }
      }
    }

    if ((hint & nsChangeHint_AddOrRemoveTransform) && frame &&
        !(hint & nsChangeHint_ReconstructFrame)) {
      for (nsIFrame* cont = frame; cont;
           cont = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
        if (cont->StyleDisplay()->HasTransform(cont)) {
          cont->AddStateBits(NS_FRAME_MAY_BE_TRANSFORMED);
        }
        // Don't remove NS_FRAME_MAY_BE_TRANSFORMED since it may still be
        // transformed by other means. It's OK to have the bit even if it's
        // not needed.
      }
    }

    if (hint & nsChangeHint_ReconstructFrame) {
      // If we ever start passing true here, be careful of restyles
      // that involve a reframe and animations.  In particular, if the
      // restyle we're processing here is an animation restyle, but
      // the style resolution we will do for the frame construction
      // happens async when we're not in an animation restyle already,
      // problems could arise.
      // We could also have problems with triggering of CSS transitions
      // on elements whose frames are reconstructed, since we depend on
      // the reconstruction happening synchronously.
      frameConstructor->RecreateFramesForContent(
        content, nsCSSFrameConstructor::InsertionKind::Sync);
    } else {
      NS_ASSERTION(frame, "This shouldn't happen");

      if (!frame->FrameMaintainsOverflow()) {
        // frame does not maintain overflow rects, so avoid calling
        // FinishAndStoreOverflow on it:
        hint &= ~(nsChangeHint_UpdateOverflow |
                  nsChangeHint_ChildrenOnlyTransform |
                  nsChangeHint_UpdatePostTransformOverflow |
                  nsChangeHint_UpdateParentOverflow);
      }

      if (!(frame->GetStateBits() & NS_FRAME_MAY_BE_TRANSFORMED)) {
        // Frame can not be transformed, and thus a change in transform will
        // have no effect and we should not use the
        // nsChangeHint_UpdatePostTransformOverflow hint.
        hint &= ~nsChangeHint_UpdatePostTransformOverflow;
      }

      if (hint & nsChangeHint_AddOrRemoveTransform) {
        // When dropping a running transform animation we will first add an
        // nsChangeHint_UpdateTransformLayer hint as part of the animation-only
        // restyle. During the subsequent regular restyle, if the animation was
        // the only reason the element had any transform applied, we will add
        // nsChangeHint_AddOrRemoveTransform as part of the regular restyle.
        //
        // With the Gecko backend, these two change hints are processed
        // after each restyle but when using the Servo backend they accumulate
        // and are processed together after we have already removed the
        // transform as part of the regular restyle. Since we don't actually
        // need the nsChangeHint_UpdateTransformLayer hint if we already have
        // a nsChangeHint_AddOrRemoveTransform hint, and since we
        // will fail an assertion in ApplyRenderingChangeToTree if we try
        // specify nsChangeHint_UpdateTransformLayer but don't have any
        // transform style, we just drop the unneeded hint here.
        hint &= ~nsChangeHint_UpdateTransformLayer;
      }

      if (hint & nsChangeHint_UpdateEffects) {
        for (nsIFrame* cont = frame; cont;
             cont = nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
          SVGObserverUtils::UpdateEffects(cont);
        }
      }
      if ((hint & nsChangeHint_InvalidateRenderingObservers) ||
          ((hint & nsChangeHint_UpdateOpacityLayer) &&
           frame->IsFrameOfType(nsIFrame::eSVG) &&
           !(frame->GetStateBits() & NS_STATE_IS_OUTER_SVG))) {
        SVGObserverUtils::InvalidateRenderingObservers(frame);
        frame->SchedulePaint();
      }
      if (hint & nsChangeHint_NeedReflow) {
        StyleChangeReflow(frame, hint);
        didReflowThisFrame = true;
      }

      // Here we need to propagate repaint frame change hint instead of update
      // opacity layer change hint when we do opacity optimization for SVG.
      // We can't do it in nsStyleEffects::CalcDifference() just like we do
      // for the optimization for 0.99 over opacity values since we have no way
      // to call nsSVGUtils::CanOptimizeOpacity() there.
      if ((hint & nsChangeHint_UpdateOpacityLayer) &&
          nsSVGUtils::CanOptimizeOpacity(frame) &&
          frame->IsFrameOfType(nsIFrame::eSVGGeometry)) {
        hint &= ~nsChangeHint_UpdateOpacityLayer;
        hint |= nsChangeHint_RepaintFrame;
      }

      if ((hint & nsChangeHint_UpdateUsesOpacity) &&
          frame->IsFrameOfType(nsIFrame::eTablePart)) {
        NS_ASSERTION(hint & nsChangeHint_UpdateOpacityLayer,
                     "should only return UpdateUsesOpacity hint "
                     "when also returning UpdateOpacityLayer hint");
        // When an internal table part (including cells) changes between
        // having opacity 1 and non-1, it changes whether its
        // backgrounds (and those of table parts inside of it) are
        // painted as part of the table's nsDisplayTableBorderBackground
        // display item, or part of its own display item.  That requires
        // invalidation, so change UpdateOpacityLayer to RepaintFrame.
        hint &= ~nsChangeHint_UpdateOpacityLayer;
        hint |= nsChangeHint_RepaintFrame;
      }

      // Opacity disables preserve-3d, so if we toggle it, then we also need
      // to update the overflow areas of all potentially affected frames.
      if ((hint & nsChangeHint_UpdateUsesOpacity) &&
          frame->StyleDisplay()->mTransformStyle == NS_STYLE_TRANSFORM_STYLE_PRESERVE_3D) {
        hint |= nsChangeHint_UpdateSubtreeOverflow;
      }

      if (hint & nsChangeHint_UpdateBackgroundPosition) {
        // For most frame types, DLBI can detect background position changes,
        // so we only need to schedule a paint.
        hint |= nsChangeHint_SchedulePaint;
        if (frame->IsFrameOfType(nsIFrame::eTablePart) ||
            frame->IsFrameOfType(nsIFrame::eMathML)) {
          // Table parts and MathML frames don't build display items for their
          // backgrounds, so DLBI can't detect background-position changes for
          // these frames. Repaint the whole frame.
          hint |= nsChangeHint_RepaintFrame;
        }
      }

      if (hint & (nsChangeHint_RepaintFrame | nsChangeHint_SyncFrameView |
                  nsChangeHint_UpdateOpacityLayer | nsChangeHint_UpdateTransformLayer |
                  nsChangeHint_ChildrenOnlyTransform | nsChangeHint_SchedulePaint)) {
        ApplyRenderingChangeToTree(presContext->PresShell(), frame, hint);
      }
      if ((hint & nsChangeHint_RecomputePosition) && !didReflowThisFrame) {
        ActiveLayerTracker::NotifyOffsetRestyle(frame);
        // It is possible for this to fall back to a reflow
        if (!RecomputePosition(frame)) {
          didReflowThisFrame = true;
        }
      }
      NS_ASSERTION(!(hint & nsChangeHint_ChildrenOnlyTransform) ||
                   (hint & nsChangeHint_UpdateOverflow),
                   "nsChangeHint_UpdateOverflow should be passed too");
      if (!didReflowThisFrame &&
          (hint & (nsChangeHint_UpdateOverflow |
                   nsChangeHint_UpdatePostTransformOverflow |
                   nsChangeHint_UpdateParentOverflow |
                   nsChangeHint_UpdateSubtreeOverflow))) {
        if (hint & nsChangeHint_UpdateSubtreeOverflow) {
          for (nsIFrame* cont = frame; cont; cont =
                 nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
            AddSubtreeToOverflowTracker(cont, mOverflowChangedTracker);
          }
          // The work we just did in AddSubtreeToOverflowTracker
          // subsumes some of the other hints:
          hint &= ~(nsChangeHint_UpdateOverflow |
                    nsChangeHint_UpdatePostTransformOverflow);
        }
        if (hint & nsChangeHint_ChildrenOnlyTransform) {
          // The overflow areas of the child frames need to be updated:
          nsIFrame* hintFrame = GetFrameForChildrenOnlyTransformHint(frame);
          nsIFrame* childFrame = hintFrame->PrincipalChildList().FirstChild();
          NS_ASSERTION(!nsLayoutUtils::GetNextContinuationOrIBSplitSibling(frame),
                       "SVG frames should not have continuations "
                       "or ib-split siblings");
          NS_ASSERTION(!nsLayoutUtils::GetNextContinuationOrIBSplitSibling(hintFrame),
                       "SVG frames should not have continuations "
                       "or ib-split siblings");
          for ( ; childFrame; childFrame = childFrame->GetNextSibling()) {
            MOZ_ASSERT(childFrame->IsFrameOfType(nsIFrame::eSVG),
                       "Not expecting non-SVG children");
            // If |childFrame| is dirty or has dirty children, we don't bother
            // updating overflows since that will happen when it's reflowed.
            if (!(childFrame->GetStateBits() &
                  (NS_FRAME_IS_DIRTY | NS_FRAME_HAS_DIRTY_CHILDREN))) {
              mOverflowChangedTracker.AddFrame(childFrame,
                                        OverflowChangedTracker::CHILDREN_CHANGED);
            }
            NS_ASSERTION(!nsLayoutUtils::GetNextContinuationOrIBSplitSibling(childFrame),
                         "SVG frames should not have continuations "
                         "or ib-split siblings");
            NS_ASSERTION(childFrame->GetParent() == hintFrame,
                         "SVG child frame not expected to have different parent");
          }
        }
        // If |frame| is dirty or has dirty children, we don't bother updating
        // overflows since that will happen when it's reflowed.
        if (!(frame->GetStateBits() &
              (NS_FRAME_IS_DIRTY | NS_FRAME_HAS_DIRTY_CHILDREN))) {
          if (hint & (nsChangeHint_UpdateOverflow |
                      nsChangeHint_UpdatePostTransformOverflow)) {
            OverflowChangedTracker::ChangeKind changeKind;
            // If we have both nsChangeHint_UpdateOverflow and
            // nsChangeHint_UpdatePostTransformOverflow,
            // CHILDREN_CHANGED is selected as it is
            // strictly stronger.
            if (hint & nsChangeHint_UpdateOverflow) {
              changeKind = OverflowChangedTracker::CHILDREN_CHANGED;
            } else {
              changeKind = OverflowChangedTracker::TRANSFORM_CHANGED;
            }
            for (nsIFrame* cont = frame; cont; cont =
                   nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
              mOverflowChangedTracker.AddFrame(cont, changeKind);
            }
          }
          // UpdateParentOverflow hints need to be processed in addition
          // to the above, since if the processing of the above hints
          // yields no change, the update will not propagate to the
          // parent.
          if (hint & nsChangeHint_UpdateParentOverflow) {
            MOZ_ASSERT(frame->GetParent(),
                       "shouldn't get style hints for the root frame");
            for (nsIFrame* cont = frame; cont; cont =
                   nsLayoutUtils::GetNextContinuationOrIBSplitSibling(cont)) {
              mOverflowChangedTracker.AddFrame(cont->GetParent(),
                                   OverflowChangedTracker::CHILDREN_CHANGED);
            }
          }
        }
      }
      if ((hint & nsChangeHint_UpdateCursor) && !didUpdateCursor) {
        presContext->PresShell()->SynthesizeMouseMove(false);
        didUpdateCursor = true;
      }
      if (hint & nsChangeHint_UpdateWidgetProperties) {
        frame->UpdateWidgetProperties();
      }
      if (hint & nsChangeHint_UpdateTableCellSpans) {
        frameConstructor->UpdateTableCellSpans(content);
      }
      if (hint & nsChangeHint_VisibilityChange) {
        frame->UpdateVisibleDescendantsState();
      }
    }
  }

  aChangeList.Clear();
}

/* static */ uint64_t
RestyleManager::GetAnimationGenerationForFrame(nsIFrame* aFrame)
{
  EffectSet* effectSet = EffectSet::GetEffectSet(aFrame);
  return effectSet ? effectSet->GetAnimationGeneration() : 0;
}

void
RestyleManager::IncrementAnimationGeneration()
{
  // We update the animation generation at start of each call to
  // ProcessPendingRestyles so we should ignore any subsequent (redundant)
  // calls that occur while we are still processing restyles.
  if (!mInStyleRefresh) {
    ++mAnimationGeneration;
  }
}

/* static */ void
RestyleManager::AddLayerChangesForAnimation(nsIFrame* aFrame,
                                            nsIContent* aContent,
                                            nsStyleChangeList&
                                              aChangeListToProcess)
{
  if (!aFrame || !aContent) {
    return;
  }

  uint64_t frameGeneration =
    RestyleManager::GetAnimationGenerationForFrame(aFrame);

  nsChangeHint hint = nsChangeHint(0);
  for (const LayerAnimationInfo::Record& layerInfo :
         LayerAnimationInfo::sRecords) {
    layers::Layer* layer =
      FrameLayerBuilder::GetDedicatedLayer(aFrame, layerInfo.mLayerType);
    if (layer && frameGeneration != layer->GetAnimationGeneration()) {
      // If we have a transform layer but don't have any transform style, we
      // probably just removed the transform but haven't destroyed the layer
      // yet. In this case we will add the appropriate change hint
      // (nsChangeHint_UpdateContainingBlock) when we compare styles so we can
      // skip adding any change hint here. (If we *were* to add
      // nsChangeHint_UpdateTransformLayer, ApplyRenderingChangeToTree would
      // complain that we're updating a transform layer without a transform).
      if (layerInfo.mLayerType == DisplayItemType::TYPE_TRANSFORM &&
          !aFrame->StyleDisplay()->HasTransformStyle()) {
        continue;
      }
      hint |= layerInfo.mChangeHint;
    }

    // We consider it's the first paint for the frame if we have an animation
    // for the property but have no layer.
    // Note that in case of animations which has properties preventing running
    // on the compositor, e.g., width or height, corresponding layer is not
    // created at all, but even in such cases, we normally set valid change
    // hint for such animations in each tick, i.e. restyles in each tick. As
    // a result, we usually do restyles for such animations in every tick on
    // the main-thread.  The only animations which will be affected by this
    // explicit change hint are animations that have opacity/transform but did
    // not have those properies just before. e.g, setting transform by
    // setKeyframes or changing target element from other target which prevents
    // running on the compositor, etc.
    if (!layer &&
        nsLayoutUtils::HasEffectiveAnimation(aFrame, layerInfo.mProperty)) {
      hint |= layerInfo.mChangeHint;
    }
  }

  if (hint) {
    aChangeListToProcess.AppendChange(aFrame, aContent, hint);
  }
}

RestyleManager::AnimationsWithDestroyedFrame::AnimationsWithDestroyedFrame(
                                                RestyleManager* aRestyleManager)
  : mRestyleManager(aRestyleManager)
  , mRestorePointer(mRestyleManager->mAnimationsWithDestroyedFrame)
{
  MOZ_ASSERT(!mRestyleManager->mAnimationsWithDestroyedFrame,
             "shouldn't construct recursively");
  mRestyleManager->mAnimationsWithDestroyedFrame = this;
}

void
RestyleManager::AnimationsWithDestroyedFrame
              ::StopAnimationsForElementsWithoutFrames()
{
  StopAnimationsWithoutFrame(mContents, CSSPseudoElementType::NotPseudo);
  StopAnimationsWithoutFrame(mBeforeContents, CSSPseudoElementType::before);
  StopAnimationsWithoutFrame(mAfterContents, CSSPseudoElementType::after);
}

void
RestyleManager::AnimationsWithDestroyedFrame
              ::StopAnimationsWithoutFrame(
                  nsTArray<RefPtr<nsIContent>>& aArray,
                  CSSPseudoElementType aPseudoType)
{
  nsAnimationManager* animationManager =
    mRestyleManager->PresContext()->AnimationManager();
  nsTransitionManager* transitionManager =
    mRestyleManager->PresContext()->TransitionManager();
  for (nsIContent* content : aArray) {
    if (aPseudoType == CSSPseudoElementType::NotPseudo) {
      if (content->GetPrimaryFrame()) {
        continue;
      }
    } else if (aPseudoType == CSSPseudoElementType::before) {
      if (nsLayoutUtils::GetBeforeFrame(content)) {
        continue;
      }
    } else if (aPseudoType == CSSPseudoElementType::after) {
      if (nsLayoutUtils::GetAfterFrame(content)) {
        continue;
      }
    }
    dom::Element* element = content->AsElement();

    animationManager->StopAnimationsForElement(element, aPseudoType);
    transitionManager->StopAnimationsForElement(element, aPseudoType);

    // All other animations should keep running but not running on the
    // *compositor* at this point.
    EffectSet* effectSet = EffectSet::GetEffectSet(element, aPseudoType);
    if (effectSet) {
      for (KeyframeEffectReadOnly* effect : *effectSet) {
        effect->ResetIsRunningOnCompositor();
      }
    }
  }
}

#ifdef DEBUG
static bool
IsAnonBox(const nsIFrame& aFrame)
{
  return aFrame.Style()->IsAnonBox();
}

static const nsIFrame*
FirstContinuationOrPartOfIBSplit(const nsIFrame* aFrame)
{
  if (!aFrame) {
    return nullptr;
  }

  return nsLayoutUtils::FirstContinuationOrIBSplitSibling(aFrame);
}

static const nsIFrame*
ExpectedOwnerForChild(const nsIFrame& aFrame)
{
  const nsIFrame* parent = aFrame.GetParent();
  if (aFrame.IsTableFrame()) {
    MOZ_ASSERT(parent->IsTableWrapperFrame());
    parent = parent->GetParent();
  }

  if (IsAnonBox(aFrame) && !aFrame.IsTextFrame()) {
    if (parent->IsLineFrame()) {
      parent = parent->GetParent();
    }
    return parent->IsViewportFrame() ?
      nullptr : FirstContinuationOrPartOfIBSplit(parent);
  }

  if (aFrame.IsBulletFrame()) {
    return FirstContinuationOrPartOfIBSplit(parent);
  }

  if (aFrame.IsLineFrame()) {
    // A ::first-line always ends up here via its block, which is therefore the
    // right expected owner.  That block can be an
    // anonymous box.  For example, we could have a ::first-line on a columnated
    // block; the blockframe is the column-content anonymous box in that case.
    // So we don't want to end up in the code below, which steps out of anon
    // boxes.  Just return the parent of the line frame, which is the block.
    return parent;
  }

  if (aFrame.IsLetterFrame()) {
    // Ditto for ::first-letter. A first-letter always arrives here via its
    // direct parent, except when it's parented to a ::first-line.
    if (parent->IsLineFrame()) {
      parent = parent->GetParent();
    }
    return FirstContinuationOrPartOfIBSplit(parent);
  }

  if (parent->IsLetterFrame()) {
    // Things never have ::first-letter as their expected parent.  Go
    // on up to the ::first-letter's parent.
    parent = parent->GetParent();
  }

  parent = FirstContinuationOrPartOfIBSplit(parent);

  // We've handled already anon boxes and bullet frames, so now we're looking at
  // a frame of a DOM element or pseudo. Hop through anon and line-boxes
  // generated by our DOM parent, and go find the owner frame for it.
  while (parent && (IsAnonBox(*parent) || parent->IsLineFrame())) {
    auto* pseudo = parent->Style()->GetPseudo();
    if (pseudo == nsCSSAnonBoxes::tableWrapper) {
      const nsIFrame* tableFrame = parent->PrincipalChildList().FirstChild();
      MOZ_ASSERT(tableFrame->IsTableFrame());
      // Handle :-moz-table and :-moz-inline-table.
      parent = IsAnonBox(*tableFrame) ? parent->GetParent() : tableFrame;
    } else {
      // We get the in-flow parent here so that we can handle the OOF anonymous
      // boxed to get the correct parent.
      parent = parent->GetInFlowParent();
    }
    parent = FirstContinuationOrPartOfIBSplit(parent);
  }

  return parent;
}

void
ServoRestyleState::AssertOwner(const ServoRestyleState& aParent) const
{
  MOZ_ASSERT(mOwner);
  MOZ_ASSERT(!mOwner->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW));
  // We allow aParent.mOwner to be null, for cases when we're not starting at
  // the root of the tree.  We also allow aParent.mOwner to be somewhere up our
  // expected owner chain not our immediate owner, which allows us creating long
  // chains of ServoRestyleStates in some cases where it's just not worth it.
#ifdef DEBUG
  if (aParent.mOwner) {
    const nsIFrame* owner = ExpectedOwnerForChild(*mOwner);
    if (owner != aParent.mOwner) {
      MOZ_ASSERT(IsAnonBox(*owner),
                 "Should only have expected owner weirdness when anon boxes are involved");
      bool found = false;
      for (; owner; owner = ExpectedOwnerForChild(*owner)) {
        if (owner == aParent.mOwner) {
          found = true;
          break;
        }
      }
      MOZ_ASSERT(found, "Must have aParent.mOwner on our expected owner chain");
    }
  }
#endif
}

nsChangeHint
ServoRestyleState::ChangesHandledFor(const nsIFrame& aFrame) const
{
  if (!mOwner) {
    MOZ_ASSERT(!mChangesHandled);
    return mChangesHandled;
  }

  MOZ_ASSERT(mOwner == ExpectedOwnerForChild(aFrame),
             "Missed some frame in the hierarchy?");
  return mChangesHandled;
}
#endif

void
ServoRestyleState::AddPendingWrapperRestyle(nsIFrame* aWrapperFrame)
{
  MOZ_ASSERT(aWrapperFrame->Style()->IsWrapperAnonBox(),
             "All our wrappers are anon boxes, and why would we restyle "
             "non-inheriting ones?");
  MOZ_ASSERT(aWrapperFrame->Style()->IsInheritingAnonBox(),
             "All our wrappers are anon boxes, and why would we restyle "
             "non-inheriting ones?");
  MOZ_ASSERT(aWrapperFrame->Style()->GetPseudo() !=
             nsCSSAnonBoxes::cellContent,
             "Someone should be using TableAwareParentFor");
  MOZ_ASSERT(aWrapperFrame->Style()->GetPseudo() !=
             nsCSSAnonBoxes::tableWrapper,
             "Someone should be using TableAwareParentFor");
  // Make sure we only add first continuations.
  aWrapperFrame = aWrapperFrame->FirstContinuation();
  nsIFrame* last = mPendingWrapperRestyles.SafeLastElement(nullptr);
  if (last == aWrapperFrame) {
    // Already queued up, nothing to do.
    return;
  }

  // Make sure to queue up parents before children.  But don't queue up
  // ancestors of non-anonymous boxes here; those are handled when we traverse
  // their non-anonymous kids.
  if (aWrapperFrame->ParentIsWrapperAnonBox()) {
    AddPendingWrapperRestyle(TableAwareParentFor(aWrapperFrame));
  }

  // If the append fails, we'll fail to restyle properly, but that's probably
  // better than crashing.
  if (mPendingWrapperRestyles.AppendElement(aWrapperFrame, fallible)) {
    aWrapperFrame->SetIsWrapperAnonBoxNeedingRestyle(true);
  }
}

void
ServoRestyleState::ProcessWrapperRestyles(nsIFrame* aParentFrame)
{
  size_t i = mPendingWrapperRestyleOffset;
  while (i < mPendingWrapperRestyles.Length()) {
    i += ProcessMaybeNestedWrapperRestyle(aParentFrame, i);
  }

  mPendingWrapperRestyles.TruncateLength(mPendingWrapperRestyleOffset);
}

size_t
ServoRestyleState::ProcessMaybeNestedWrapperRestyle(nsIFrame* aParent,
                                                    size_t aIndex)
{
  // The frame at index aIndex is something we should restyle ourselves, but
  // following frames may need separate ServoRestyleStates to restyle.
  MOZ_ASSERT(aIndex < mPendingWrapperRestyles.Length());

  nsIFrame* cur = mPendingWrapperRestyles[aIndex];
  MOZ_ASSERT(cur->Style()->IsWrapperAnonBox());

  // Where is cur supposed to inherit from?  From its parent frame, except in
  // the case when cur is a table, in which case it should be its grandparent.
  // Also, not in the case when the resulting frame would be a first-line; in
  // that case we should be inheriting from the block, and the first-line will
  // do its fixup later if needed.
  //
  // Note that after we do all that fixup the parent we get might still not be
  // aParent; for example aParent could be a scrollframe, in which case we
  // should inherit from the scrollcontent frame.  Or the parent might be some
  // continuation of aParent.
  //
  // Try to assert as much as we can about the parent we actually end up using
  // without triggering bogus asserts in all those various edge cases.
  nsIFrame* parent = cur->GetParent();
  if (cur->IsTableFrame()) {
    MOZ_ASSERT(parent->IsTableWrapperFrame());
    parent = parent->GetParent();
  }
  if (parent->IsLineFrame()) {
    parent = parent->GetParent();
  }
  MOZ_ASSERT(FirstContinuationOrPartOfIBSplit(parent) == aParent ||
             (parent->Style()->IsInheritingAnonBox() &&
              parent->GetContent() == aParent->GetContent()));

  // Now "this" is a ServoRestyleState for aParent, so if parent is not a next
  // continuation (possibly across ib splits) of aParent we need a new
  // ServoRestyleState for the kid.
  Maybe<ServoRestyleState> parentRestyleState;
  nsIFrame* parentForRestyle =
    nsLayoutUtils::FirstContinuationOrIBSplitSibling(parent);
  if (parentForRestyle != aParent) {
    parentRestyleState.emplace(*parentForRestyle, *this, nsChangeHint_Empty,
                               Type::InFlow);
  }
  ServoRestyleState& curRestyleState =
    parentRestyleState ? *parentRestyleState : *this;

  // This frame may already have been restyled.  Even if it has, we can't just
  // return, because the next frame may be a kid of it that does need restyling.
  if (cur->IsWrapperAnonBoxNeedingRestyle()) {
    parentForRestyle->UpdateStyleOfChildAnonBox(cur, curRestyleState);
    cur->SetIsWrapperAnonBoxNeedingRestyle(false);
  }

  size_t numProcessed = 1;

  // Note: no overflow possible here, since aIndex < length.
  if (aIndex + 1 < mPendingWrapperRestyles.Length()) {
    nsIFrame* next = mPendingWrapperRestyles[aIndex + 1];
    if (TableAwareParentFor(next) == cur &&
        next->IsWrapperAnonBoxNeedingRestyle()) {
      // It might be nice if we could do better than nsChangeHint_Empty.  On
      // the other hand, presumably our mChangesHandled already has the bits
      // we really want here so in practice it doesn't matter.
      ServoRestyleState childState(*cur, curRestyleState, nsChangeHint_Empty,
                                   Type::InFlow,
                                   /* aAssertWrapperRestyleLength = */ false);
      numProcessed += childState.ProcessMaybeNestedWrapperRestyle(cur,
                                                                  aIndex + 1);
    }
  }

  return numProcessed;
}

nsIFrame*
ServoRestyleState::TableAwareParentFor(const nsIFrame* aChild)
{
  // We want to get the anon box parent for aChild. where aChild has
  // ParentIsWrapperAnonBox().
  //
  // For the most part this is pretty straightforward, but there are two
  // wrinkles.  First, if aChild is a table, then we really want the parent of
  // its table wrapper.
  if (aChild->IsTableFrame()) {
    aChild = aChild->GetParent();
    MOZ_ASSERT(aChild->IsTableWrapperFrame());
  }

  nsIFrame* parent = aChild->GetParent();
  // Now if parent is a cell-content frame, we actually want the cellframe.
  if (parent->Style()->GetPseudo() == nsCSSAnonBoxes::cellContent) {
    parent = parent->GetParent();
  } else if (parent->IsTableWrapperFrame()) {
    // Must be a caption.  In that case we want the table here.
    MOZ_ASSERT(aChild->StyleDisplay()->mDisplay == StyleDisplay::TableCaption);
    parent = parent->PrincipalChildList().FirstChild();
  }
  return parent;
}

void
RestyleManager::PostRestyleEvent(Element* aElement,
                                      nsRestyleHint aRestyleHint,
                                      nsChangeHint aMinChangeHint)
{
  MOZ_ASSERT(!(aMinChangeHint & nsChangeHint_NeutralChange),
             "Didn't expect explicit change hints to be neutral!");
  if (MOZ_UNLIKELY(IsDisconnected()) ||
      MOZ_UNLIKELY(PresContext()->PresShell()->IsDestroying())) {
    return;
  }

  // We allow posting restyles from within change hint handling, but not from
  // within the restyle algorithm itself.
  MOZ_ASSERT(!ServoStyleSet::IsInServoTraversal());

  if (aRestyleHint == 0 && !aMinChangeHint) {
    return; // Nothing to do.
  }

  // Assuming the restyle hints will invalidate cached style for
  // getComputedStyle, since we don't know if any of the restyling that we do
  // would affect undisplayed elements.
  if (aRestyleHint) {
    IncrementUndisplayedRestyleGeneration();
  }

  // Processing change hints sometimes causes new change hints to be generated,
  // and very occasionally, additional restyle hints. We collect the change
  // hints manually to avoid re-traversing the DOM to find them.
  if (mReentrantChanges && !aRestyleHint) {
    mReentrantChanges->AppendElement(ReentrantChange { aElement, aMinChangeHint });
    return;
  }

  if (aRestyleHint & ~eRestyle_AllHintsWithAnimations) {
    mHaveNonAnimationRestyles = true;
  }

  if (aRestyleHint & eRestyle_LaterSiblings) {
    aRestyleHint &= ~eRestyle_LaterSiblings;

    nsRestyleHint siblingHint = eRestyle_Subtree;
    Element* current = aElement->GetNextElementSibling();
    while (current) {
      Servo_NoteExplicitHints(current, siblingHint, nsChangeHint(0));
      current = current->GetNextElementSibling();
    }
  }

  if (aRestyleHint || aMinChangeHint) {
    Servo_NoteExplicitHints(aElement, aRestyleHint, aMinChangeHint);
  }
}

void
RestyleManager::PostRestyleEventForCSSRuleChanges()
{
  mRestyleForCSSRuleChanges = true;
  mPresContext->PresShell()->EnsureStyleFlush();
}

void
RestyleManager::PostRestyleEventForAnimations(
  Element* aElement,
  CSSPseudoElementType aPseudoType,
  nsRestyleHint aRestyleHint)
{
  Element* elementToRestyle =
    EffectCompositor::GetElementToRestyle(aElement, aPseudoType);

  if (!elementToRestyle) {
    // FIXME: Bug 1371107: When reframing happens,
    // EffectCompositor::mElementsToRestyle still has unbinded old pseudo
    // element. We should drop it.
    return;
  }

  AutoRestyleTimelineMarker marker(mPresContext->GetDocShell(),
                                   true /* animation-only */);
  Servo_NoteExplicitHints(elementToRestyle, aRestyleHint, nsChangeHint(0));
}

void
RestyleManager::RebuildAllStyleData(nsChangeHint aExtraHint,
                                         nsRestyleHint aRestyleHint)
{
   // NOTE(emilio): GeckoRestlyeManager does a sync style flush, which seems not
   // to be needed in my testing.
  PostRebuildAllStyleDataEvent(aExtraHint, aRestyleHint);
}

void
RestyleManager::PostRebuildAllStyleDataEvent(nsChangeHint aExtraHint,
                                                  nsRestyleHint aRestyleHint)
{
  // NOTE(emilio): The semantics of these methods are quite funny, in the sense
  // that we're not supposed to need to rebuild the actual stylist data.
  //
  // That's handled as part of the MediumFeaturesChanged stuff, if needed.
  StyleSet()->ClearCachedStyleData();

  DocumentStyleRootIterator iter(mPresContext->Document());
  while (Element* root = iter.GetNextStyleRoot()) {
    PostRestyleEvent(root, aRestyleHint, aExtraHint);
  }

  // TODO(emilio, bz): Extensions can add/remove stylesheets that can affect
  // non-inheriting anon boxes. It's not clear if we want to support that, but
  // if we do, we need to re-selector-match them here.
}

/* static */ void
RestyleManager::ClearServoDataFromSubtree(Element* aElement, IncludeRoot aIncludeRoot)
{
  if (aElement->HasServoData()) {
    StyleChildrenIterator it(aElement);
    for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
      if (n->IsElement()) {
        ClearServoDataFromSubtree(n->AsElement(), IncludeRoot::Yes);
      }
    }
  }

  if (MOZ_LIKELY(aIncludeRoot == IncludeRoot::Yes)) {
    aElement->ClearServoData();
    MOZ_ASSERT(!aElement->HasAnyOfFlags(Element::kAllServoDescendantBits | NODE_NEEDS_FRAME));
    MOZ_ASSERT(aElement != aElement->OwnerDoc()->GetServoRestyleRoot());
  }
}

/* static */ void
RestyleManager::ClearRestyleStateFromSubtree(Element* aElement)
{
  if (aElement->HasAnyOfFlags(Element::kAllServoDescendantBits)) {
    StyleChildrenIterator it(aElement);
    for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
      if (n->IsElement()) {
        ClearRestyleStateFromSubtree(n->AsElement());
      }
    }
  }

  bool wasRestyled;
  Unused << Servo_TakeChangeHint(aElement, &wasRestyled);
  aElement->UnsetFlags(Element::kAllServoDescendantBits);
}

/**
 * This struct takes care of encapsulating some common state that text nodes may
 * need to track during the post-traversal.
 *
 * This is currently used to properly compute change hints when the parent
 * element of this node is a display: contents node, and also to avoid computing
 * the style for text children more than once per element.
 */
struct RestyleManager::TextPostTraversalState
{
public:
  TextPostTraversalState(Element& aParentElement,
                         ComputedStyle* aParentContext,
                         bool aDisplayContentsParentStyleChanged,
                         ServoRestyleState& aParentRestyleState)
    : mParentElement(aParentElement)
    , mParentContext(aParentContext)
    , mParentRestyleState(aParentRestyleState)
    , mStyle(nullptr)
    , mShouldPostHints(aDisplayContentsParentStyleChanged)
    , mShouldComputeHints(aDisplayContentsParentStyleChanged)
    , mComputedHint(nsChangeHint_Empty)
  {}

  nsStyleChangeList& ChangeList() { return mParentRestyleState.ChangeList(); }

  ComputedStyle& ComputeStyle(nsIContent* aTextNode)
  {
    if (!mStyle) {
      mStyle = mParentRestyleState.StyleSet().ResolveStyleForText(
        aTextNode, &ParentStyle());
    }
    MOZ_ASSERT(mStyle);
    return *mStyle;
  }

  void ComputeHintIfNeeded(nsIContent* aContent,
                           nsIFrame* aTextFrame,
                           ComputedStyle& aNewStyle)
  {
    MOZ_ASSERT(aTextFrame);
    MOZ_ASSERT(aNewStyle.GetPseudo() == nsCSSAnonBoxes::mozText);

    if (MOZ_LIKELY(!mShouldPostHints)) {
      return;
    }

    ComputedStyle* oldStyle = aTextFrame->Style();
    MOZ_ASSERT(oldStyle->GetPseudo() == nsCSSAnonBoxes::mozText);

    // We rely on the fact that all the text children for the same element share
    // style to avoid recomputing style differences for all of them.
    //
    // TODO(emilio): The above may not be true for ::first-{line,letter}, but
    // we'll cross that bridge when we support those in stylo.
    if (mShouldComputeHints) {
      mShouldComputeHints = false;
      uint32_t equalStructs;
      mComputedHint = oldStyle->CalcStyleDifference(&aNewStyle, &equalStructs);
      mComputedHint = NS_RemoveSubsumedHints(
        mComputedHint, mParentRestyleState.ChangesHandledFor(*aTextFrame));
    }

    if (mComputedHint) {
      mParentRestyleState.ChangeList().AppendChange(
        aTextFrame, aContent, mComputedHint);
    }
  }

private:
  ComputedStyle& ParentStyle() {
    if (!mParentContext) {
      mLazilyResolvedParentContext =
        mParentRestyleState.StyleSet().ResolveServoStyle(&mParentElement);
      mParentContext = mLazilyResolvedParentContext;
    }
    return *mParentContext;
  }

  Element& mParentElement;
  ComputedStyle* mParentContext;
  RefPtr<ComputedStyle> mLazilyResolvedParentContext;
  ServoRestyleState& mParentRestyleState;
  RefPtr<ComputedStyle> mStyle;
  bool mShouldPostHints;
  bool mShouldComputeHints;
  nsChangeHint mComputedHint;
};

static void
UpdateBackdropIfNeeded(nsIFrame* aFrame,
                       ServoStyleSet& aStyleSet,
                       nsStyleChangeList& aChangeList)
{
  const nsStyleDisplay* display = aFrame->Style()->StyleDisplay();
  if (display->mTopLayer != NS_STYLE_TOP_LAYER_TOP) {
    return;
  }

  // Elements in the top layer are guaranteed to have absolute or fixed
  // position per https://fullscreen.spec.whatwg.org/#new-stacking-layer.
  MOZ_ASSERT(display->IsAbsolutelyPositionedStyle());

  nsIFrame* backdropPlaceholder =
    aFrame->GetChildList(nsIFrame::kBackdropList).FirstChild();
  if (!backdropPlaceholder) {
    return;
  }

  MOZ_ASSERT(backdropPlaceholder->IsPlaceholderFrame());
  nsIFrame* backdropFrame =
    nsPlaceholderFrame::GetRealFrameForPlaceholder(backdropPlaceholder);
  MOZ_ASSERT(backdropFrame->IsBackdropFrame());
  MOZ_ASSERT(backdropFrame->Style()->GetPseudoType() ==
             CSSPseudoElementType::backdrop);

  RefPtr<ComputedStyle> newStyle =
    aStyleSet.ResolvePseudoElementStyle(aFrame->GetContent()->AsElement(),
                                        CSSPseudoElementType::backdrop,
                                        aFrame->Style(),
                                        /* aPseudoElement = */ nullptr);

  // NOTE(emilio): We can't use the changes handled for the owner of the
  // backdrop frame, since it's out of flow, and parented to the viewport or
  // canvas frame (depending on the `position` value).
  MOZ_ASSERT(backdropFrame->GetParent()->IsViewportFrame() ||
             backdropFrame->GetParent()->IsCanvasFrame());
  nsTArray<nsIFrame*> wrappersToRestyle;
  ServoRestyleState state(aStyleSet, aChangeList, wrappersToRestyle);
  nsIFrame::UpdateStyleOfOwnedChildFrame(backdropFrame, newStyle, state);
}

static void
UpdateFirstLetterIfNeeded(nsIFrame* aFrame, ServoRestyleState& aRestyleState)
{
  MOZ_ASSERT(!aFrame->IsFrameOfType(nsIFrame::eBlockFrame),
             "You're probably duplicating work with UpdatePseudoElementStyles!");
  if (!aFrame->HasFirstLetterChild()) {
    return;
  }

  // We need to find the block the first-letter is associated with so we can
  // find the right element for the first-letter's style resolution.  Might as
  // well just delegate the whole thing to that block.
  nsIFrame* block = aFrame->GetParent();
  while (!block->IsFrameOfType(nsIFrame::eBlockFrame)) {
    block = block->GetParent();
  }

  static_cast<nsBlockFrame*>(block->FirstContinuation())->
    UpdateFirstLetterStyle(aRestyleState);
}

static void
UpdateOneAdditionalComputedStyle(nsIFrame* aFrame,
                                uint32_t aIndex,
                                ComputedStyle& aOldContext,
                                ServoRestyleState& aRestyleState)
{
  auto pseudoType = aOldContext.GetPseudoType();
  MOZ_ASSERT(pseudoType != CSSPseudoElementType::NotPseudo);
  MOZ_ASSERT(
      !nsCSSPseudoElements::PseudoElementSupportsUserActionState(pseudoType));

  RefPtr<ComputedStyle> newStyle =
    aRestyleState.StyleSet().ResolvePseudoElementStyle(
        aFrame->GetContent()->AsElement(),
        pseudoType,
        aFrame->Style(),
        /* aPseudoElement = */ nullptr);

  uint32_t equalStructs; // Not used, actually.
  nsChangeHint childHint =
    aOldContext.CalcStyleDifference(newStyle, &equalStructs);
  if (!aFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW)) {
    childHint = NS_RemoveSubsumedHints(
        childHint, aRestyleState.ChangesHandledFor(*aFrame));
  }

  if (childHint) {
    if (childHint & nsChangeHint_ReconstructFrame) {
      // If we generate a reconstruct here, remove any non-reconstruct hints we
      // may have already generated for this content.
      aRestyleState.ChangeList().PopChangesForContent(aFrame->GetContent());
    }
    aRestyleState.ChangeList().AppendChange(
        aFrame, aFrame->GetContent(), childHint);
  }

  aFrame->SetAdditionalComputedStyle(aIndex, newStyle);
}

static void
UpdateAdditionalComputedStyles(nsIFrame* aFrame,
                              ServoRestyleState& aRestyleState)
{
  MOZ_ASSERT(aFrame);
  MOZ_ASSERT(aFrame->GetContent() && aFrame->GetContent()->IsElement());

  // FIXME(emilio): Consider adding a bit or something to avoid the initial
  // virtual call?
  uint32_t index = 0;
  while (auto* oldStyle = aFrame->GetAdditionalComputedStyle(index)) {
    UpdateOneAdditionalComputedStyle(
        aFrame, index++, *oldStyle, aRestyleState);
  }
}

static void
UpdateFramePseudoElementStyles(nsIFrame* aFrame,
                               ServoRestyleState& aRestyleState)
{
  if (aFrame->IsFrameOfType(nsIFrame::eBlockFrame)) {
    static_cast<nsBlockFrame*>(aFrame)->UpdatePseudoElementStyles(aRestyleState);
  } else {
    UpdateFirstLetterIfNeeded(aFrame, aRestyleState);
  }

  UpdateBackdropIfNeeded(
    aFrame, aRestyleState.StyleSet(), aRestyleState.ChangeList());
}

enum class ServoPostTraversalFlags : uint32_t
{
  Empty = 0,
  // Whether parent was restyled.
  ParentWasRestyled = 1 << 0,
  // Skip sending accessibility notifications for all descendants.
  SkipA11yNotifications = 1 << 1,
  // Always send accessibility notifications if the element is shown.
  // The SkipA11yNotifications flag above overrides this flag.
  SendA11yNotificationsIfShown = 1 << 2,
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(ServoPostTraversalFlags)

// Send proper accessibility notifications and return post traversal
// flags for kids.
static ServoPostTraversalFlags
SendA11yNotifications(nsPresContext* aPresContext,
                      Element* aElement,
                      ComputedStyle* aOldComputedStyle,
                      ComputedStyle* aNewComputedStyle,
                      ServoPostTraversalFlags aFlags)
{
  using Flags = ServoPostTraversalFlags;
  MOZ_ASSERT(!(aFlags & Flags::SkipA11yNotifications) ||
             !(aFlags & Flags::SendA11yNotificationsIfShown),
             "The two a11y flags should never be set together");

#ifdef ACCESSIBILITY
  nsAccessibilityService* accService = GetAccService();
  if (!accService) {
    // If we don't have accessibility service, accessibility is not
    // enabled. Just skip everything.
    return Flags::Empty;
  }
  if (aFlags & Flags::SkipA11yNotifications) {
    // Propogate the skipping flag to descendants.
    return Flags::SkipA11yNotifications;
  }

  bool needsNotify = false;
  bool isVisible = aNewComputedStyle->StyleVisibility()->IsVisible();
  if (aFlags & Flags::SendA11yNotificationsIfShown) {
    if (!isVisible) {
      // Propagate the sending-if-shown flag to descendants.
      return Flags::SendA11yNotificationsIfShown;
    }
    // We have asked accessibility service to remove the whole subtree
    // of element which becomes invisible from the accessible tree, but
    // this element is visible, so we need to add it back.
    needsNotify = true;
  } else {
    // If we shouldn't skip in any case, we need to check whether our
    // own visibility has changed.
    bool wasVisible = aOldComputedStyle->StyleVisibility()->IsVisible();
    needsNotify = wasVisible != isVisible;
  }

  if (needsNotify) {
    nsIPresShell* presShell = aPresContext->PresShell();
    if (isVisible) {
      accService->ContentRangeInserted(
        presShell, aElement, aElement->GetNextSibling());
      // We are adding the subtree. Accessibility service would handle
      // descendants, so we should just skip them from notifying.
      return Flags::SkipA11yNotifications;
    }
    // Remove the subtree of this invisible element, and ask any shown
    // descendant to add themselves back.
    accService->ContentRemoved(presShell, aElement);
    return Flags::SendA11yNotificationsIfShown;
  }
#endif

  return Flags::Empty;
}

bool
RestyleManager::ProcessPostTraversal(
  Element* aElement,
  ComputedStyle* aParentContext,
  ServoRestyleState& aRestyleState,
  ServoPostTraversalFlags aFlags)
{
  nsIFrame* styleFrame = nsLayoutUtils::GetStyleFrame(aElement);
  nsIFrame* primaryFrame = aElement->GetPrimaryFrame();

  // NOTE(emilio): This is needed because for table frames the bit is set on the
  // table wrapper (which is the primary frame), not on the table itself.
  const bool isOutOfFlow =
    primaryFrame &&
    primaryFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW);

  // Grab the change hint from Servo.
  bool wasRestyled;
  nsChangeHint changeHint =
    static_cast<nsChangeHint>(Servo_TakeChangeHint(aElement, &wasRestyled));

  // We should really fix the weird primary frame mapping for image maps
  // (bug 135040)...
  if (styleFrame && styleFrame->GetContent() != aElement) {
    MOZ_ASSERT(static_cast<nsImageFrame*>(do_QueryFrame(styleFrame)));
    styleFrame = nullptr;
  }

  // Handle lazy frame construction by posting a reconstruct for any lazily-
  // constructed roots.
  if (aElement->HasFlag(NODE_NEEDS_FRAME)) {
    changeHint |= nsChangeHint_ReconstructFrame;
    MOZ_ASSERT(!styleFrame);
  }

  if (styleFrame) {
    MOZ_ASSERT(primaryFrame);

    nsIFrame* maybeAnonBoxChild;
    if (isOutOfFlow) {
      maybeAnonBoxChild = primaryFrame->GetPlaceholderFrame();
    } else {
      maybeAnonBoxChild = primaryFrame;
      changeHint = NS_RemoveSubsumedHints(
        changeHint, aRestyleState.ChangesHandledFor(*styleFrame));
    }

    // If the parent wasn't restyled, the styles of our anon box parents won't
    // change either.
    if ((aFlags & ServoPostTraversalFlags::ParentWasRestyled) &&
        maybeAnonBoxChild->ParentIsWrapperAnonBox()) {
      aRestyleState.AddPendingWrapperRestyle(
        ServoRestyleState::TableAwareParentFor(maybeAnonBoxChild));
    }
  }

  // Although we shouldn't generate non-ReconstructFrame hints for elements with
  // no frames, we can still get them here if they were explicitly posted by
  // PostRestyleEvent, such as a RepaintFrame hint when a :link changes to be
  // :visited.  Skip processing these hints if there is no frame.
  if ((styleFrame || (changeHint & nsChangeHint_ReconstructFrame)) && changeHint) {
    aRestyleState.ChangeList().AppendChange(styleFrame, aElement, changeHint);
  }

  // If our change hint is reconstruct, we delegate to the frame constructor,
  // which consumes the new style and expects the old style to be on the frame.
  //
  // XXXbholley: We should teach the frame constructor how to clear the dirty
  // descendants bit to avoid the traversal here.
  if (changeHint & nsChangeHint_ReconstructFrame) {
    ClearRestyleStateFromSubtree(aElement);
    return true;
  }

  // TODO(emilio): We could avoid some refcount traffic here, specially in the
  // ComputedStyle case, which uses atomic refcounting.
  //
  // Hold the ComputedStyle alive, because it could become a dangling pointer
  // during the replacement. In practice it's not a huge deal, but better not
  // playing with dangling pointers if not needed.
  //
  // NOTE(emilio): We could keep around the old computed style for display:
  // contents elements too, but we don't really need it right now.
  RefPtr<ComputedStyle> oldOrDisplayContentsStyle =
    styleFrame ? styleFrame->Style() : nullptr;

  MOZ_ASSERT(!(styleFrame && Servo_Element_IsDisplayContents(aElement)),
             "display: contents node has a frame, yet we didn't reframe it"
             " above?");
  const bool isDisplayContents =
    !styleFrame && Servo_Element_IsDisplayContents(aElement);
  if (isDisplayContents) {
    oldOrDisplayContentsStyle =
      aRestyleState.StyleSet().ResolveServoStyle(aElement);
  }

  Maybe<ServoRestyleState> thisFrameRestyleState;
  if (styleFrame) {
    auto type = isOutOfFlow
      ? ServoRestyleState::Type::OutOfFlow
      : ServoRestyleState::Type::InFlow;

    thisFrameRestyleState.emplace(*styleFrame, aRestyleState, changeHint, type);
  }

  // We can't really assume as used changes from display: contents elements (or
  // other elements without frames).
  ServoRestyleState& childrenRestyleState =
    thisFrameRestyleState ? *thisFrameRestyleState : aRestyleState;

  RefPtr<ComputedStyle> upToDateContext =
    wasRestyled
      ? aRestyleState.StyleSet().ResolveServoStyle(aElement)
      : oldOrDisplayContentsStyle;

  ServoPostTraversalFlags childrenFlags =
    wasRestyled ? ServoPostTraversalFlags::ParentWasRestyled
                : ServoPostTraversalFlags::Empty;

  if (wasRestyled && oldOrDisplayContentsStyle) {
    MOZ_ASSERT(styleFrame || isDisplayContents);

    // Note that upToDateContext could be the same as oldOrDisplayContentsStyle,
    // but it doesn't matter, since the only point of it is calling FinishStyle
    // on the relevant structs, and those don't matter for display: contents.
    upToDateContext->ResolveSameStructsAs(oldOrDisplayContentsStyle);

    // We want to walk all the continuations here, even the ones with different
    // styles.  In practice, the only reason we get continuations with different
    // styles here is ::first-line (::first-letter never affects element
    // styles).  But in that case, newStyle is the right context for the
    // _later_ continuations anyway (the ones not affected by ::first-line), not
    // the earlier ones, so there is no point stopping right at the point when
    // we'd actually be setting the right ComputedStyle.
    //
    // This does mean that we may be setting the wrong ComputedStyle on our
    // initial continuations; ::first-line fixes that up after the fact.
    for (nsIFrame* f = styleFrame; f; f = f->GetNextContinuation()) {
      MOZ_ASSERT_IF(f != styleFrame, !f->GetAdditionalComputedStyle(0));
      f->SetComputedStyle(upToDateContext);
    }

    if (styleFrame) {
      UpdateAdditionalComputedStyles(styleFrame, aRestyleState);
    }

    if (!aElement->GetParent()) {
      // This is the root.  Update styles on the viewport as needed.
      ViewportFrame* viewport =
        do_QueryFrame(mPresContext->PresShell()->GetRootFrame());
      if (viewport) {
        // NB: The root restyle state, not the one for our children!
        viewport->UpdateStyle(aRestyleState);
      }
    }

    // Some changes to animations don't affect the computed style and yet still
    // require the layer to be updated. For example, pausing an animation via
    // the Web Animations API won't affect an element's style but still
    // requires to update the animation on the layer.
    //
    // We can sometimes reach this when the animated style is being removed.
    // Since AddLayerChangesForAnimation checks if |styleFrame| has a transform
    // style or not, we need to call it *after* setting |newStyle| to
    // |styleFrame| to ensure the animated transform has been removed first.
    AddLayerChangesForAnimation(
      styleFrame, aElement, aRestyleState.ChangeList());

    childrenFlags |= SendA11yNotifications(mPresContext,
                                           aElement,
                                           oldOrDisplayContentsStyle,
                                           upToDateContext,
                                           aFlags);
  }

  const bool traverseElementChildren =
    aElement->HasAnyOfFlags(Element::kAllServoDescendantBits);
  const bool traverseTextChildren =
    wasRestyled || aElement->HasFlag(NODE_DESCENDANTS_NEED_FRAMES);
  bool recreatedAnyContext = wasRestyled;
  if (traverseElementChildren || traverseTextChildren) {
    StyleChildrenIterator it(aElement);
    TextPostTraversalState textState(*aElement,
                                     upToDateContext,
                                     isDisplayContents && wasRestyled,
                                     childrenRestyleState);
    for (nsIContent* n = it.GetNextChild(); n; n = it.GetNextChild()) {
      if (traverseElementChildren && n->IsElement()) {
        recreatedAnyContext |= ProcessPostTraversal(n->AsElement(),
                                                    upToDateContext,
                                                    childrenRestyleState,
                                                    childrenFlags);
      } else if (traverseTextChildren && n->IsNodeOfType(nsINode::eTEXT)) {
        recreatedAnyContext |= ProcessPostTraversalForText(n, textState,
                                                           childrenRestyleState,
                                                           childrenFlags);
      }
    }
  }

  // We want to update frame pseudo-element styles after we've traversed our
  // kids, because some of those updates (::first-line/::first-letter) need to
  // modify the styles of the kids, and the child traversal above would just
  // clobber those modifications.
  if (styleFrame) {
    if (wasRestyled) {
      // Make sure to update anon boxes and pseudo bits after updating text,
      // otherwise ProcessPostTraversalForText could clobber first-letter
      // styles, for example.
      styleFrame->UpdateStyleOfOwnedAnonBoxes(childrenRestyleState);
    }
    // Process anon box wrapper frames before ::first-line bits, but _after_
    // owned anon boxes, since the children wrapper anon boxes could be
    // inheriting from our own owned anon boxes.
    childrenRestyleState.ProcessWrapperRestyles(styleFrame);
    if (wasRestyled) {
      UpdateFramePseudoElementStyles(styleFrame, childrenRestyleState);
    } else if (traverseElementChildren &&
               styleFrame->IsFrameOfType(nsIFrame::eBlockFrame)) {
      // Even if we were not restyled, if we're a block with a first-line and
      // one of our descendant elements which is on the first line was restyled,
      // we need to update the styles of things on the first line, because
      // they're wrong now.
      //
      // FIXME(bz) Could we do better here?  For example, could we keep track of
      // frames that are "block with a ::first-line so we could avoid
      // IsFrameOfType() and digging about for the first-line frame if not?
      // Could we keep track of whether the element children we actually restyle
      // are affected by first-line?  Something else?  Bug 1385443 tracks making
      // this better.
      nsIFrame* firstLineFrame =
        static_cast<nsBlockFrame*>(styleFrame)->GetFirstLineFrame();
      if (firstLineFrame) {
        for (nsIFrame* kid : firstLineFrame->PrincipalChildList()) {
          ReparentComputedStyleForFirstLine(kid);
        }
      }
    }
  }

  aElement->UnsetFlags(Element::kAllServoDescendantBits);
  return recreatedAnyContext;
}

bool
RestyleManager::ProcessPostTraversalForText(
    nsIContent* aTextNode,
    TextPostTraversalState& aPostTraversalState,
    ServoRestyleState& aRestyleState,
    ServoPostTraversalFlags aFlags)
{
  // Handle lazy frame construction.
  if (aTextNode->HasFlag(NODE_NEEDS_FRAME)) {
    aPostTraversalState.ChangeList().AppendChange(
      nullptr, aTextNode, nsChangeHint_ReconstructFrame);
    return true;
  }

  // Handle restyle.
  nsIFrame* primaryFrame = aTextNode->GetPrimaryFrame();
  if (!primaryFrame) {
    return false;
  }

  // If the parent wasn't restyled, the styles of our anon box parents won't
  // change either.
  if ((aFlags & ServoPostTraversalFlags::ParentWasRestyled) &&
      primaryFrame->ParentIsWrapperAnonBox()) {
    aRestyleState.AddPendingWrapperRestyle(
      ServoRestyleState::TableAwareParentFor(primaryFrame));
  }

  ComputedStyle& newStyle = aPostTraversalState.ComputeStyle(aTextNode);
  aPostTraversalState.ComputeHintIfNeeded(aTextNode, primaryFrame, newStyle);

  // We want to walk all the continuations here, even the ones with different
  // styles.  In practice, the only reasons we get continuations with different
  // styles are ::first-line and ::first-letter.  But in those cases,
  // newStyle is the right context for the _later_ continuations anyway (the
  // ones not affected by ::first-line/::first-letter), not the earlier ones,
  // so there is no point stopping right at the point when we'd actually be
  // setting the right ComputedStyle.
  //
  // This does mean that we may be setting the wrong ComputedStyle on our
  // initial continuations; ::first-line/::first-letter fix that up after the
  // fact.
  for (nsIFrame* f = primaryFrame; f; f = f->GetNextContinuation()) {
    f->SetComputedStyle(&newStyle);
  }

  return true;
}

void
RestyleManager::ClearSnapshots()
{
  for (auto iter = mSnapshots.Iter(); !iter.Done(); iter.Next()) {
    iter.Key()->UnsetFlags(ELEMENT_HAS_SNAPSHOT | ELEMENT_HANDLED_SNAPSHOT);
    iter.Remove();
  }
}

ServoElementSnapshot&
RestyleManager::SnapshotFor(Element* aElement)
{
  MOZ_ASSERT(!mInStyleRefresh);

  // NOTE(emilio): We can handle snapshots from a one-off restyle of those that
  // we do to restyle stuff for reconstruction, for example.
  //
  // It seems to be the case that we always flush in between that happens and
  // the next attribute change, so we can assert that we haven't handled the
  // snapshot here yet. If this assertion didn't hold, we'd need to unset that
  // flag from here too.
  //
  // Can't wait to make ProcessPendingRestyles the only entry-point for styling,
  // so this becomes much easier to reason about. Today is not that day though.
  MOZ_ASSERT(aElement->HasServoData());
  MOZ_ASSERT(!aElement->HasFlag(ELEMENT_HANDLED_SNAPSHOT));

  ServoElementSnapshot* snapshot = mSnapshots.LookupOrAdd(aElement, aElement);
  aElement->SetFlags(ELEMENT_HAS_SNAPSHOT);

  // Now that we have a snapshot, make sure a restyle is triggered.
  aElement->NoteDirtyForServo();
  return *snapshot;
}

void
RestyleManager::DoProcessPendingRestyles(ServoTraversalFlags aFlags)
{
  nsPresContext* presContext = PresContext();

  MOZ_ASSERT(presContext->Document(), "No document?  Pshaw!");
  // FIXME(emilio): In the "flush animations" case, ideally, we should only
  // recascade animation styles running on the compositor, so we shouldn't care
  // about other styles, or new rules that apply to the page...
  //
  // However, that's not true as of right now, see bug 1388031 and bug 1388692.
  MOZ_ASSERT((aFlags & ServoTraversalFlags::FlushThrottledAnimations) ||
             !presContext->HasPendingMediaQueryUpdates(),
             "Someone forgot to update media queries?");
  MOZ_ASSERT(!nsContentUtils::IsSafeToRunScript(), "Missing a script blocker!");
  MOZ_ASSERT(!mInStyleRefresh, "Reentrant call?");


  if (MOZ_UNLIKELY(!presContext->PresShell()->DidInitialize())) {
    // PresShell::FlushPendingNotifications doesn't early-return in the case
    // where the PresShell hasn't yet been initialized (and therefore we haven't
    // yet done the initial style traversal of the DOM tree). We should arguably
    // fix up the callers and assert against this case, but we just detect and
    // handle it for now.
    return;
  }

  // Create a AnimationsWithDestroyedFrame during restyling process to
  // stop animations and transitions on elements that have no frame at the end
  // of the restyling process.
  AnimationsWithDestroyedFrame animationsWithDestroyedFrame(this);

  ServoStyleSet* styleSet = StyleSet();
  nsIDocument* doc = presContext->Document();

  // Ensure the refresh driver is active during traversal to avoid mutating
  // mActiveTimer and mMostRecentRefresh time.
  presContext->RefreshDriver()->MostRecentRefresh();


  // Perform the Servo traversal, and the post-traversal if required. We do this
  // in a loop because certain rare paths in the frame constructor (like
  // uninstalling XBL bindings) can trigger additional style validations.
  mInStyleRefresh = true;
  if (mHaveNonAnimationRestyles) {
    ++mAnimationGeneration;
  }

  if (mRestyleForCSSRuleChanges) {
    aFlags |= ServoTraversalFlags::ForCSSRuleChanges;
  }

  while (styleSet->StyleDocument(aFlags)) {
    ClearSnapshots();

    nsStyleChangeList currentChanges;
    bool anyStyleChanged = false;

    // Recreate styles , and queue up change hints (which also handle lazy frame
    // construction).
    {
      AutoRestyleTimelineMarker marker(presContext->GetDocShell(), false);
      DocumentStyleRootIterator iter(doc->GetServoRestyleRoot());
      while (Element* root = iter.GetNextStyleRoot()) {
        nsTArray<nsIFrame*> wrappersToRestyle;
        ServoRestyleState state(*styleSet, currentChanges, wrappersToRestyle);
        ServoPostTraversalFlags flags = ServoPostTraversalFlags::Empty;
        anyStyleChanged |= ProcessPostTraversal(root, nullptr, state, flags);
      }
    }

    doc->ClearServoRestyleRoot();

    // Process the change hints.
    //
    // Unfortunately, the frame constructor can generate new change hints while
    // processing existing ones. We redirect those into a secondary queue and
    // iterate until there's nothing left.
    {
      AutoTimelineMarker marker(
        presContext->GetDocShell(), "StylesApplyChanges");
      ReentrantChangeList newChanges;
      mReentrantChanges = &newChanges;
      while (!currentChanges.IsEmpty()) {
        ProcessRestyledFrames(currentChanges);
        MOZ_ASSERT(currentChanges.IsEmpty());
        for (ReentrantChange& change: newChanges)  {
          if (!(change.mHint & nsChangeHint_ReconstructFrame) &&
              !change.mContent->GetPrimaryFrame()) {
            // SVG Elements post change hints without ensuring that the primary
            // frame will be there after that (see bug 1366142).
            //
            // Just ignore those, since we can't really process them.
            continue;
          }
          currentChanges.AppendChange(change.mContent->GetPrimaryFrame(),
                                      change.mContent, change.mHint);
        }
        newChanges.Clear();
      }
      mReentrantChanges = nullptr;
    }

    if (anyStyleChanged) {
      // Maybe no styles changed when:
      //
      //  * Only explicit change hints were posted in the first place.
      //  * When an attribute or state change in the content happens not to need
      //    a restyle after all.
      //
      // In any case, we don't need to increment the restyle generation in that
      // case.
      IncrementRestyleGeneration();
    }
  }

  doc->ClearServoRestyleRoot();

  FlushOverflowChangedTracker();

  ClearSnapshots();
  styleSet->AssertTreeIsClean();
  mHaveNonAnimationRestyles = false;
  mRestyleForCSSRuleChanges = false;
  mInStyleRefresh = false;

  // Now that everything has settled, see if we have enough free rule nodes in
  // the tree to warrant sweeping them.
  styleSet->MaybeGCRuleTree();

  // Note: We are in the scope of |animationsWithDestroyedFrame|, so
  //       |mAnimationsWithDestroyedFrame| is still valid.
  MOZ_ASSERT(mAnimationsWithDestroyedFrame);
  mAnimationsWithDestroyedFrame->StopAnimationsForElementsWithoutFrames();
}

#ifdef DEBUG
static void
VerifyFlatTree(const nsIContent& aContent)
{
  StyleChildrenIterator iter(&aContent);

  for (auto* content = iter.GetNextChild();
       content;
       content = iter.GetNextChild()) {
    MOZ_ASSERT(content->GetFlattenedTreeParentNodeForStyle() == &aContent);
    VerifyFlatTree(*content);
  }
}
#endif

void
RestyleManager::ProcessPendingRestyles()
{
#ifdef DEBUG
  if (auto* root = mPresContext->Document()->GetRootElement()) {
    VerifyFlatTree(*root);
  }
#endif

  DoProcessPendingRestyles(ServoTraversalFlags::Empty);
}

void
RestyleManager::ProcessAllPendingAttributeAndStateInvalidations()
{
  if (mSnapshots.IsEmpty()) {
    return;
  }
  for (auto iter = mSnapshots.Iter(); !iter.Done(); iter.Next()) {
    // Servo data for the element might have been dropped. (e.g. by removing
    // from its document)
    if (iter.Key()->HasFlag(ELEMENT_HAS_SNAPSHOT)) {
      Servo_ProcessInvalidations(StyleSet()->RawSet(), iter.Key(), &mSnapshots);
    }
  }
  ClearSnapshots();
}

bool
RestyleManager::HasPendingRestyleAncestor(Element* aElement) const
{
  return Servo_HasPendingRestyleAncestor(aElement);
}

void
RestyleManager::UpdateOnlyAnimationStyles()
{
  bool doCSS = PresContext()->EffectCompositor()->HasPendingStyleUpdates();
  if (!doCSS) {
    return;
  }

  DoProcessPendingRestyles(ServoTraversalFlags::FlushThrottledAnimations);
}

void
RestyleManager::ContentStateChanged(nsIContent* aContent,
                                    EventStates aChangedBits)
{
  MOZ_ASSERT(!mInStyleRefresh);

  if (!aContent->IsElement()) {
    return;
  }

  Element* aElement = aContent->AsElement();
  if (!aElement->HasServoData()) {
    return;
  }

  nsChangeHint changeHint;
  ContentStateChangedInternal(aElement, aChangedBits, &changeHint);

  // Don't bother taking a snapshot if no rules depend on these state bits.
  //
  // We always take a snapshot for the LTR/RTL event states, since Servo doesn't
  // track those bits in the same way, and we know that :dir() rules are always
  // present in UA style sheets.
  if (!aChangedBits.HasAtLeastOneOfStates(DIRECTION_STATES) &&
      !StyleSet()->HasStateDependency(*aElement, aChangedBits)) {
    return;
  }

  ServoElementSnapshot& snapshot = SnapshotFor(aElement);
  EventStates previousState = aElement->StyleState() ^ aChangedBits;
  snapshot.AddState(previousState);

  if (changeHint) {
    Servo_NoteExplicitHints(aElement, nsRestyleHint(0), changeHint);
  }

  // Assuming we need to invalidate cached style in getComputedStyle for
  // undisplayed elements, since we don't know if it is needed.
  IncrementUndisplayedRestyleGeneration();
}

static inline bool
AttributeInfluencesOtherPseudoClassState(const Element& aElement,
                                         const nsAtom* aAttribute)
{
  // We must record some state for :-moz-browser-frame and
  // :-moz-table-border-nonzero.
  if (aAttribute == nsGkAtoms::mozbrowser) {
    return aElement.IsAnyOfHTMLElements(nsGkAtoms::iframe, nsGkAtoms::frame);
  }

  if (aAttribute == nsGkAtoms::border) {
    return aElement.IsHTMLElement(nsGkAtoms::table);
  }

  return false;
}

static inline bool
NeedToRecordAttrChange(const ServoStyleSet& aStyleSet,
                       const Element& aElement,
                       int32_t aNameSpaceID,
                       nsAtom* aAttribute,
                       bool* aInfluencesOtherPseudoClassState)
{
  *aInfluencesOtherPseudoClassState =
    AttributeInfluencesOtherPseudoClassState(aElement, aAttribute);

  // If the attribute influences one of the pseudo-classes that are backed by
  // attributes, we just record it.
  if (*aInfluencesOtherPseudoClassState) {
    return true;
  }

  // We assume that id and class attributes are used in class/id selectors, and
  // thus record them.
  //
  // TODO(emilio): We keep a filter of the ids in use somewhere in the StyleSet,
  // presumably we could try to filter the old and new id, but it's not clear
  // it's worth it.
  if (aNameSpaceID == kNameSpaceID_None &&
      (aAttribute == nsGkAtoms::id || aAttribute == nsGkAtoms::_class)) {
    return true;
  }

  // We always record lang="", even though we force a subtree restyle when it
  // changes, since it can change how its siblings match :lang(..) due to
  // selectors like :lang(..) + div.
  if (aAttribute == nsGkAtoms::lang) {
    return true;
  }

  // Otherwise, just record the attribute change if a selector in the page may
  // reference it from an attribute selector.
  return aStyleSet.MightHaveAttributeDependency(aElement, aAttribute);
}

void
RestyleManager::AttributeWillChange(Element* aElement,
                                    int32_t aNameSpaceID,
                                    nsAtom* aAttribute,
                                    int32_t aModType,
                                    const nsAttrValue* aNewValue)
{
  TakeSnapshotForAttributeChange(aElement, aNameSpaceID, aAttribute);
}

void
RestyleManager::ClassAttributeWillBeChangedBySMIL(Element* aElement)
{
  TakeSnapshotForAttributeChange(aElement, kNameSpaceID_None,
                                 nsGkAtoms::_class);
}

void
RestyleManager::TakeSnapshotForAttributeChange(Element* aElement,
                                               int32_t aNameSpaceID,
                                               nsAtom* aAttribute)
{
  MOZ_ASSERT(!mInStyleRefresh);

  if (!aElement->HasServoData()) {
    return;
  }

  bool influencesOtherPseudoClassState;
  if (!NeedToRecordAttrChange(*StyleSet(),
                              *aElement,
                              aNameSpaceID,
                              aAttribute,
                              &influencesOtherPseudoClassState)) {
    return;
  }

  // We cannot tell if the attribute change will affect the styles of
  // undisplayed elements, because we don't actually restyle those elements
  // during the restyle traversal. So just assume that the attribute change can
  // cause the style to change.
  IncrementUndisplayedRestyleGeneration();

  // Some other random attribute changes may also affect the transitions,
  // so we also set this true here.
  mHaveNonAnimationRestyles = true;

  ServoElementSnapshot& snapshot = SnapshotFor(aElement);
  snapshot.AddAttrs(aElement, aNameSpaceID, aAttribute);

  if (influencesOtherPseudoClassState) {
    snapshot.AddOtherPseudoClassState(aElement);
  }
}

// For some attribute changes we must restyle the whole subtree:
//
// * <td> is affected by the cellpadding on its ancestor table
// * lwtheme and lwthemetextcolor on root element of XUL document
//   affects all descendants due to :-moz-lwtheme* pseudo-classes
// * lang="" and xml:lang="" can affect all descendants due to :lang()
//
static inline bool
AttributeChangeRequiresSubtreeRestyle(const Element& aElement, nsAtom* aAttr)
{
  if (aAttr == nsGkAtoms::cellpadding) {
    return aElement.IsHTMLElement(nsGkAtoms::table);
  }
  if (aAttr == nsGkAtoms::lwtheme ||
      aAttr == nsGkAtoms::lwthemetextcolor) {
    return aElement.GetNameSpaceID() == kNameSpaceID_XUL &&
      &aElement == aElement.OwnerDoc()->GetRootElement();
  }

  return aAttr == nsGkAtoms::lang;
}

void
RestyleManager::AttributeChanged(Element* aElement,
                                 int32_t aNameSpaceID,
                                 nsAtom* aAttribute,
                                 int32_t aModType,
                                 const nsAttrValue* aOldValue)
{
  MOZ_ASSERT(!mInStyleRefresh);

  auto changeHint = nsChangeHint(0);
  auto restyleHint = nsRestyleHint(0);

  changeHint |= aElement->GetAttributeChangeHint(aAttribute, aModType);

  if (aAttribute == nsGkAtoms::style) {
    restyleHint |= eRestyle_StyleAttribute;
  } else if (AttributeChangeRequiresSubtreeRestyle(*aElement, aAttribute)) {
    restyleHint |= eRestyle_Subtree;
  } else if (aElement->IsAttributeMapped(aAttribute)) {
    restyleHint |= eRestyle_Self;
  }

  if (nsIFrame* primaryFrame = aElement->GetPrimaryFrame()) {
    // See if we have appearance information for a theme.
    const nsStyleDisplay* disp = primaryFrame->StyleDisplay();
    if (disp->mAppearance) {
      nsITheme* theme = PresContext()->GetTheme();
      if (theme && theme->ThemeSupportsWidget(PresContext(), primaryFrame,
                                              disp->mAppearance)) {
        bool repaint = false;
        theme->WidgetStateChanged(primaryFrame, disp->mAppearance,
                                  aAttribute, &repaint, aOldValue);
        if (repaint) {
          changeHint |= nsChangeHint_RepaintFrame;
        }
      }
    }

    primaryFrame->AttributeChanged(aNameSpaceID, aAttribute, aModType);
  }

  if (restyleHint || changeHint) {
    Servo_NoteExplicitHints(aElement, restyleHint, changeHint);
  }

  if (restyleHint) {
    // Assuming we need to invalidate cached style in getComputedStyle for
    // undisplayed elements, since we don't know if it is needed.
    IncrementUndisplayedRestyleGeneration();

    // If we change attributes, we have to mark this to be true, so we will
    // increase the animation generation for the new created transition if any.
    mHaveNonAnimationRestyles = true;
  }
}

void
RestyleManager::ReparentComputedStyleForFirstLine(nsIFrame* aFrame)
{
  // This is only called when moving frames in or out of the first-line
  // pseudo-element (or one of its descendants).  We can't say much about
  // aFrame's ancestors, unfortunately (e.g. during a dynamic insert into
  // something inside an inline-block on the first line the ancestors could be
  // totally arbitrary), but we will definitely find a line frame on the
  // ancestor chain.  Note that the lineframe may not actually be the one that
  // corresponds to ::first-line; when we're moving _out_ of the ::first-line it
  // will be one of the continuations instead.
#ifdef DEBUG
  {
    nsIFrame* f = aFrame->GetParent();
    while (f && !f->IsLineFrame()) {
      f = f->GetParent();
    }
    MOZ_ASSERT(f, "Must have found a first-line frame");
  }
#endif

  DoReparentComputedStyleForFirstLine(aFrame, *StyleSet());
}

void
RestyleManager::DoReparentComputedStyleForFirstLine(nsIFrame* aFrame,
                                                    ServoStyleSet& aStyleSet)
{
  if (aFrame->IsBackdropFrame()) {
    // Style context of backdrop frame has no parent style, and thus we do not
    // need to reparent it.
    return;
  }

  if (aFrame->IsPlaceholderFrame()) {
    // Also reparent the out-of-flow and all its continuations.  We're doing
    // this to match Gecko for now, but it's not clear that this behavior is
    // correct per spec.  It's certainly pretty odd for out-of-flows whose
    // containing block is not within the first line.
    //
    // Right now we're somewhat inconsistent in this testcase:
    //
    //  <style>
    //    div { color: orange; clear: left; }
    //    div::first-line { color: blue; }
    //  </style>
    //  <div>
    //    <span style="float: left">What color is this text?</span>
    //  </div>
    //  <div>
    //    <span><span style="float: left">What color is this text?</span></span>
    //  </div>
    //
    // We make the first float orange and the second float blue.  On the other
    // hand, if the float were within an inline-block that was on the first
    // line, arguably it _should_ inherit from the ::first-line...
    nsIFrame* outOfFlow =
      nsPlaceholderFrame::GetRealFrameForPlaceholder(aFrame);
    MOZ_ASSERT(outOfFlow, "no out-of-flow frame");
    for (; outOfFlow; outOfFlow = outOfFlow->GetNextContinuation()) {
      DoReparentComputedStyleForFirstLine(outOfFlow, aStyleSet);
    }
  }

  // FIXME(emilio): This is the only caller of GetParentComputedStyle, let's try
  // to remove it?
  nsIFrame* providerFrame;
  ComputedStyle* newParentStyle =
    aFrame->GetParentComputedStyle(&providerFrame);
  // If our provider is our child, we want to reparent it first, because we
  // inherit style from it.
  bool isChild = providerFrame && providerFrame->GetParent() == aFrame;
  nsIFrame* providerChild = nullptr;
  if (isChild) {
    DoReparentComputedStyleForFirstLine(providerFrame, aStyleSet);
    // Get the style again after ReparentComputedStyle() which might have
    // changed it.
    newParentStyle = providerFrame->Style();
    providerChild = providerFrame;
    MOZ_ASSERT(!providerFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW),
               "Out of flow provider?");
  }

  if (!newParentStyle) {
    // No need to do anything here for this frame, but we should still reparent
    // its descendants, because those may have styles that inherit from the
    // parent of this frame (e.g. non-anonymous columns in an anonymous
    // colgroup).
    MOZ_ASSERT(aFrame->Style()->IsNonInheritingAnonBox(),
               "Why did this frame not end up with a parent context?");
    ReparentFrameDescendants(aFrame, providerChild, aStyleSet);
    return;
  }

  bool isElement = aFrame->GetContent()->IsElement();

  // We probably don't want to initiate transitions from ReparentComputedStyle,
  // since we call it during frame construction rather than in response to
  // dynamic changes.
  // Also see the comment at the start of
  // nsTransitionManager::ConsiderInitiatingTransition.
  //
  // We don't try to do the fancy copying from previous continuations that
  // GeckoRestyleManager does here, because that relies on knowing the parents
  // of ComputedStyles, and we don't know those.
  ComputedStyle* oldStyle = aFrame->Style();
  Element* ourElement =
    oldStyle->GetPseudoType() == CSSPseudoElementType::NotPseudo &&
    isElement ?
      aFrame->GetContent()->AsElement() :
      nullptr;
  ComputedStyle* newParent = newParentStyle;

  ComputedStyle* newParentIgnoringFirstLine;
  if (newParent->GetPseudoType() == CSSPseudoElementType::firstLine) {
    MOZ_ASSERT(providerFrame && providerFrame->GetParent()->
               IsFrameOfType(nsIFrame::eBlockFrame),
               "How could we get a ::first-line parent style without having "
               "a ::first-line provider frame?");
    // If newParent is a ::first-line style, get the parent blockframe, and then
    // correct it for our pseudo as needed (e.g. stepping out of anon boxes).
    // Use the resulting style for the "parent style ignoring ::first-line".
    nsIFrame* blockFrame = providerFrame->GetParent();
    nsIFrame* correctedFrame =
      nsFrame::CorrectStyleParentFrame(blockFrame, oldStyle->GetPseudo());
    newParentIgnoringFirstLine = correctedFrame->Style();
  } else {
    newParentIgnoringFirstLine = newParent;
  }

  if (!providerFrame) {
    // No providerFrame means we inherited from a display:contents thing.  Our
    // layout parent style is the style of our nearest ancestor frame.  But we have
    // to be careful to do that with our placeholder, not with us, if we're out of
    // flow.
    if (aFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW)) {
      aFrame->GetPlaceholderFrame()->GetLayoutParentStyleForOutOfFlow(&providerFrame);
    } else {
      providerFrame = nsFrame::CorrectStyleParentFrame(aFrame->GetParent(),
                                                       oldStyle->GetPseudo());
    }
  }
  ComputedStyle* layoutParent = providerFrame->Style();

  RefPtr<ComputedStyle> newStyle =
    aStyleSet.ReparentComputedStyle(oldStyle,
                                    newParent,
                                    newParentIgnoringFirstLine,
                                    layoutParent,
                                    ourElement);
  aFrame->SetComputedStyle(newStyle);

  // This logic somewhat mirrors the logic in
  // RestyleManager::ProcessPostTraversal.
  if (isElement) {
    // We can't use UpdateAdditionalComputedStyles as-is because it needs a
    // ServoRestyleState and maintaining one of those during a _frametree_
    // traversal is basically impossible.
    uint32_t index = 0;
    while (auto* oldAdditionalStyle = aFrame->GetAdditionalComputedStyle(index)) {
      RefPtr<ComputedStyle> newAdditionalContext =
        aStyleSet.ReparentComputedStyle(oldAdditionalStyle,
                                        newStyle,
                                        newStyle,
                                        newStyle,
                                        nullptr);
      aFrame->SetAdditionalComputedStyle(index, newAdditionalContext);
      ++index;
    }
  }

  // Generally, owned anon boxes are our descendants.  The only exceptions are
  // tables (for the table wrapper) and inline frames (for the block part of the
  // block-in-inline split).  We're going to update our descendants when looping
  // over kids, and we don't want to update the block part of a block-in-inline
  // split if the inline is on the first line but the block is not (and if the
  // block is, it's the child of something else on the first line and will get
  // updated as a child).  And given how this method ends up getting called, if
  // we reach here for a table frame, we are already in the middle of
  // reparenting the table wrapper frame.  So no need to
  // UpdateStyleOfOwnedAnonBoxes() here.

  ReparentFrameDescendants(aFrame, providerChild, aStyleSet);

  // We do not need to do the equivalent of UpdateFramePseudoElementStyles,
  // because those are hadled by our descendant walk.
}

void
RestyleManager::ReparentFrameDescendants(nsIFrame* aFrame,
                                         nsIFrame* aProviderChild,
                                         ServoStyleSet& aStyleSet)
{
  nsIFrame::ChildListIterator lists(aFrame);
  for (; !lists.IsDone(); lists.Next()) {
    for (nsIFrame* child : lists.CurrentList()) {
      // only do frames that are in flow
      if (!(child->GetStateBits() & NS_FRAME_OUT_OF_FLOW) &&
          child != aProviderChild) {
        DoReparentComputedStyleForFirstLine(child, aStyleSet);
      }
    }
  }
}

} // namespace mozilla