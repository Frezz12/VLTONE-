// The VST3 interface IIDs, defined exactly once.
//
// The SDK declares each interface's `iid` in its header but leaves the
// *definition* to whichever translation unit is compiled with `INIT_CLASS_IID`.
// In the full SDK that unit lives in `public.sdk`, which a host does not
// otherwise need, so this is our own equivalent: define the macro, include
// every interface header we might touch, and let the SDK's own
// `DECLARE_CLASS_IID` emit the definitions.
//
// Two things about this file are load-bearing:
//
//  * `INIT_CLASS_IID` must be defined **before the first include**. The switch
//    lives in an `#ifdef` inside `funknown.h`, so it is read when that header
//    is first preprocessed — including anything that pulls in `funknown.h`
//    first leaves `DECLARE_CLASS_IID` permanently in its non-defining form and
//    this file compiles to an empty object.
//  * The SDK's own `coreiids.cpp` is deliberately **not** in the build. It
//    defines the base interfaces' IIDs, which this file also defines through
//    the transitive includes below; having both is a handful of duplicate
//    symbols at link. One file defining all of them is the simpler half.
#define INIT_CLASS_IID

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/icloneable.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/iupdatehandler.h>

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstautomationstate.h>
#include <pluginterfaces/vst/ivstchannelcontextinfo.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstcontextmenu.h>
#include <pluginterfaces/vst/ivstdataexchange.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstinterappaudio.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstmidilearn.h>
#include <pluginterfaces/vst/ivstmidimapping2.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstparameterfunctionname.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/ivstplugview.h>
#include <pluginterfaces/vst/ivstprefetchablesupport.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstremapparamid.h>
#include <pluginterfaces/vst/ivstrepresentation.h>
#include <pluginterfaces/vst/ivsttestplugprovider.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/vstpresetkeys.h>
#include <pluginterfaces/vst/vstpshpack4.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/vst/vsttypes.h>
