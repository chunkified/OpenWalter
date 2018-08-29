#include "walterTranslator.h"

#include <ai_array.h>
#include <ai_nodes.h>
#include <ai_ray.h>
#include "rdoArnold.h"

#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <vector>

#include "attributes/AttrHelper.h"
#include "common/UtilityFunctions.h"
#include "mayaUtils.h"

#include <maya/MBoundingBox.h>
#include <maya/MDagPath.h>
#include <maya/MFileObject.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnStringArrayData.h>
#include <maya/MPlugArray.h>
#include <maya/MRenderUtil.h>
#include <maya/MSelectionList.h>
#include <sstream>

#ifdef _WIN32
    #include <platform/win32/dirent.h>
    #define LIBEXT MString(".dll")
#else
    #include <sys/types.h>
    #include <dirent.h>
    #include <dlfcn.h>
#endif


#if WALTER_MTOA_VERSION >= 10400
CWalterStandinTranslator::CWalterStandinTranslator() :
    m_arnoldRootNode(NULL)
{
}

AtNode* CWalterStandinTranslator::GetArnoldRootNode()
{
    return m_arnoldRootNode;
}

#else
void CWalterStandinTranslator::Update(AtNode* procedural)
{
    ExportProcedural(procedural, true);
}

#endif

AtNode*  CWalterStandinTranslator::CreateArnoldNodes()
{
    m_arnoldRootNode = AddArnoldNode(RDO_WALTER_PROC);
    return m_arnoldRootNode;
}


void CWalterStandinTranslator::ProcessRenderFlagsCustom(AtNode* node)
{
    AiNodeSetByte(node, "visibility", ComputeVisibility());

    MPlug plug;

    if(m_DagNode.findPlug("aiSelfShadows").asBool() == false)
          ProcessParameter(node, "self_shadows", AI_TYPE_BOOLEAN, "aiSelfShadows");

    if(m_DagNode.findPlug("aiOpaque").asBool() == false)
          ProcessParameter(node, "opaque", AI_TYPE_BOOLEAN, "aiOpaque");

        if(m_DagNode.findPlug("aiMatte").asBool() == true)
          ProcessParameter(node, "matte", AI_TYPE_BOOLEAN, "aiMatte");

    if(m_DagNode.findPlug("receiveShadows").asBool() == false)
          ProcessParameter(node, "receive_shadows", AI_TYPE_BOOLEAN, "receiveShadows");
    MStatus status;

    plug = FindMayaPlug("aiSssSetname", &status);
    if (status && !plug.isNull())
    {
        MString setname = plug.asString();
        if (setname != "")
        {
            AiNodeDeclareConstant(node, "sss_setname", AI_TYPE_STRING);
            AiNodeSetStr(node, "sss_setname", setname.asChar());
        }
    }

    ExportTraceSets(node, FindMayaPlug("aiTraceSets"));
}

void CWalterStandinTranslator::Export(AtNode* procedural)
{
    ExportProcedural(procedural, false);
}

void CWalterStandinTranslator::ExportProcedural(AtNode* procedural, bool update)
{
    MStatus stat;
    m_DagNode.setObject(m_dagPath.node());

#if WALTER_MTOA_VERSION >= 10400
    ExportMatrix(procedural);
#else
    ExportMatrix(procedural, 0);
#endif
    ProcessRenderFlagsCustom(procedural);

    if (!update)
    {
        // Export material assigned on the node.
        ExportStandinsShaders(procedural);

        AiNodeSetStr(procedural, "name", m_dagPath.partialPathName().asChar());

        // Try to find the procedural name on the standin node.
        MString procLib = m_DagNode.findPlug("arnoldProcedural").asString();
        if (!procLib.length())
        {
            // Use the standard name if not found.
            procLib = MString("walterProcedural") + LIBEXT;
        }

        #if AI_VERSION_ARCH_NUM==4
        AiNodeSetStr(procedural, "dso", procLib.asChar() );

        bool loadAtInit =  m_DagNode.findPlug("loadAtInit").asBool();
        AiNodeSetBool(procedural, "load_at_init", loadAtInit);
        #endif

        // Split the string with ":" symbol, expand all the filenames and join
        // it back.
        std::string cacheFileName(
                m_DagNode.findPlug("cacheFileName").asString().asChar());

        // Split the line with ":" symbol and put everything to Alembic. We
        // don't use MString::split() because it skips empty lines. We need
        // everything, otherwise we have errors in matching visibilities and
        // layers.
        std::vector<std::string> archives;
        // TODO: Windows paths
        boost::split(archives, cacheFileName, boost::is_any_of(":"));

        MString abcfiles;
        MPlug renderables = m_DagNode.findPlug("renderables");
        for (unsigned i=0; i<archives.size(); i++)
        {
            MString archive(archives[i].c_str());

            if (!renderables.isNull())
            {
                // Skip it if it's not renderable
                MPlug renderable = renderables.elementByLogicalIndex(i);
                if (!renderable.asBool())
                {
                    continue;
                }
            }

            // Compute the resolved filename
            MFileObject fileObject;
            fileObject.setRawFullName(archive);
            fileObject.setResolveMethod(MFileObject::kInputFile);

            if (abcfiles.length())
            {
                abcfiles += ":";
            }

            MString resolved = fileObject.resolvedFullName();
            if (resolved.length())
            {
                abcfiles += resolved.expandEnvironmentVariablesAndTilde();
            }
            else
            {
                abcfiles += archive.expandEnvironmentVariablesAndTilde();
            }
        }

        // TODO: do we still need this block? We export nodes directly from our
        // connections. We might need this for compatibility with the first
        // versions.
        MPlug shaders = m_DagNode.findPlug("shaders", &stat);
        if(stat == MS::kSuccess)
        {
            for (unsigned int i=0;i<shaders.numElements();++i)
            {
                MPlug plug = shaders.elementByPhysicalIndex(i, &stat);
                if(stat == MS::kSuccess)
                {
                    MPlugArray connections;
                    plug.connectedTo(connections, true, false, &stat);
                    if(stat == MS::kSuccess)
                        for (unsigned int k=0; k<connections.length(); ++k)
                        {
                            MPlug sgPlug = connections[k];
                            if (sgPlug.node().apiType() == MFn::kShadingEngine || sgPlug.node().apiType() == MFn::kDisplacementShader)
                            {
#if WALTER_MTOA_VERSION >= 10400
                                ExportConnectedNode(sgPlug);
#else
                                ExportNode(sgPlug);
#endif
                            }
                        }
                }

            }
        }

        MPlug usdSessionLayer = m_DagNode.findPlug("USDSessionLayer");
        MPlug usdVariantsLayer = m_DagNode.findPlug("USDVariantsLayer");
        MPlug usdMayaStateLayer = m_DagNode.findPlug("USDMayaStateLayer");
        MPlug usdPurposeLayer = m_DagNode.findPlug("USDPurposeLayer");
        MPlug usdVisibilityLayer = m_DagNode.findPlug("USDVisibilityLayer");

        ExportMayaShadingGraph();

        // It's a list of materials from the Alembic file.
        // We don't use MStringArray because MStringArray::indexOf() absent in
        // our Maya SDK for unknown reason.
        std::vector<MString> embeddedShaderList;
        const MPlug embeddedShaders = m_DagNode.findPlug("embeddedShaders");
        if (!embeddedShaders.isNull())
        {
            unsigned int numElements = embeddedShaders.numElements();
            embeddedShaderList.reserve(numElements);
            for (unsigned int i=0; i<numElements; i++)
            {
                embeddedShaderList.push_back(
                        embeddedShaders.elementByLogicalIndex(i).asString());
            }
        }

#if WALTER_MTOA_VERSION >= 10400
        ExportFrame(procedural);
#else
        ExportFrame(procedural, 0);
#endif

        #if AI_VERSION_ARCH_NUM==4
        AiNodeDeclare(procedural, "objectPath", "constant STRING");
        #endif
        AiNodeSetStr(procedural, "objectPath", "/");

        static const MTime sec(1.0, MTime::kSeconds);
        float fps = sec.as(MTime::uiUnit());
        AiNodeDeclare(procedural, "fps", "constant FLOAT");
        AiNodeSetFlt(procedural, "fps", fps);

        #if AI_VERSION_ARCH_NUM==4
        AiNodeDeclare(procedural, "filePaths", "constant STRING");
        #endif
        AiNodeSetStr(procedural, "filePaths", abcfiles.asChar());

        // Output the USD session layer if exists.
        if (!usdSessionLayer.isNull())
        {
            MString sessionLayer = usdSessionLayer.asString();
            if (sessionLayer.length())
            {
#if AI_VERSION_ARCH_NUM == 4
                AiNodeDeclare(procedural, "sessionLayer", "constant STRING");
#endif
                AiNodeSetStr(procedural, "sessionLayer", sessionLayer.asChar());
            }
        }

        // Output the USD variants layer if exists.
        if (!usdVariantsLayer.isNull())
        {
            MString variantsLayer = usdVariantsLayer.asString();
            if (variantsLayer.length())
            {
#if AI_VERSION_ARCH_NUM == 4
                AiNodeDeclare(procedural, "variantsLayer", "constant STRING");
#endif
                AiNodeSetStr(procedural, "variantsLayer", variantsLayer.asChar());
            }
        }

        // Output the USD puropse layer if exists.
        if (!usdPurposeLayer.isNull())
        {
            MString purposeLayer = usdPurposeLayer.asString();
            if (purposeLayer.length())
            {
#if AI_VERSION_ARCH_NUM == 4
                AiNodeDeclare(procedural, "purposeLayer", "constant STRING");
#endif
                AiNodeSetStr(procedural, "purposeLayer", purposeLayer.asChar());
            }
        }

        // Output the USD session layer if exists.
        if (!usdMayaStateLayer.isNull())
        {
            MString mayaStateLayer = usdMayaStateLayer.asString();
            if (mayaStateLayer.length())
            {
#if AI_VERSION_ARCH_NUM == 4
                AiNodeDeclare(procedural, "mayaStateLayer", "constant STRING");
#endif
                AiNodeSetStr(
                    procedural, "mayaStateLayer", mayaStateLayer.asChar());
            }
        }

        // Output the USD visiblity layer if exists.
        if (!usdVisibilityLayer.isNull())
        {
            MString visibilityLayer = usdVisibilityLayer.asString();
            if (visibilityLayer.length())
            {
#if AI_VERSION_ARCH_NUM == 4
                AiNodeDeclare(procedural, "visibilityLayer", "constant STRING");
#endif
                AiNodeSetStr(procedural, "visibilityLayer", visibilityLayer.asChar());
            }
        }

#if AI_VERSION_ARCH_NUM == 4
        ExportBoundingBox(procedural);
#else
        // MTOA does this for every node.
        if (RequiresMotionData())
        {
            double motionStart, motionEnd;
            GetSessionOptions().GetMotionRange(motionStart, motionEnd);
            AiNodeSetFlt(procedural, "motion_start", (float)motionStart);
            AiNodeSetFlt(procedural, "motion_end", (float)motionEnd);
        }
#endif
    }
}


void CWalterStandinTranslator::ExportShaders()
{
  ExportStandinsShaders(GetArnoldRootNode());
}


void CWalterStandinTranslator::ExportStandinsShaders(AtNode* procedural)
{
    int instanceNum = m_dagPath.isInstanced() ? m_dagPath.instanceNumber() : 0;

    MPlug shadingGroupPlug = GetNodeShadingGroup(m_dagPath.node(), instanceNum);
    if (!shadingGroupPlug.isNull())
    {

        AtNode *shader =
#if WALTER_MTOA_VERSION >= 10400
            ExportConnectedNode(shadingGroupPlug);
#else
            ExportNode(shadingGroupPlug);
#endif
        if (shader != NULL)
        {
            // We can't put it to "shader" attribute because in this way we will
            // not be able to assign different shader.
            const char* shaderAttribute = "walterStandinShader";
            const AtNodeEntry* nodeEntry = AiNodeGetNodeEntry(procedural);
            const AtParamEntry* paramEntry =
                AiNodeEntryLookUpParameter(nodeEntry, shaderAttribute);

            if (!paramEntry)
            {
                // The param doesn't exists, add it!
                AiNodeDeclare(
                        procedural,
                        shaderAttribute,
                        "constant NODE");
            }

            AiNodeSetPtr(procedural, shaderAttribute, shader);
        }
    }
}

#if WALTER_MTOA_VERSION >= 10400
void CWalterStandinTranslator::ExportMotion(AtNode* anode)
#else
void CWalterStandinTranslator::ExportMotion(AtNode* anode, unsigned int step)
#endif
{
    // Check if motionblur is enabled and early out if it's not.
    if (!IsMotionBlurEnabled())
    {
        return;
    }

#if WALTER_MTOA_VERSION >= 10400
    ExportMatrix(anode);
    ExportFrame(anode);
#else
    ExportMatrix(anode, step);
    ExportFrame(anode, step);
#endif
}

#if AI_VERSION_ARCH_NUM==4
void CWalterStandinTranslator::ExportBoundingBox(AtNode* procedural)
{
    MBoundingBox boundingBox = m_DagNode.boundingBox();
    MPoint bbMin = boundingBox.min();
    MPoint bbMax = boundingBox.max();

    float minCoords[4];
    float maxCoords[4];

    bbMin.get(minCoords);
    bbMax.get(maxCoords);

    AiNodeSetPnt(procedural, "min", minCoords[0], minCoords[1], minCoords[2]);
    AiNodeSetPnt(procedural, "max", maxCoords[0], maxCoords[1], maxCoords[2]);
}
#endif


void CWalterStandinTranslator::NodeInitializer(CAbTranslator context)
{
    CExtensionAttrHelper helper(context.maya, RDO_WALTER_PROC);
    CShapeTranslator::MakeCommonAttributes(helper);

    #if AI_VERSION_ARCH_NUM==4
    CAttrData data;

    data.defaultValue.BOOL = false;
    data.name = "overrideGlobalShader";
    data.shortName = "ogs";
    helper.MakeInputBoolean(data) ;

    data.defaultValue.BOOL = true;
    data.name = "loadAtInit";
    data.shortName = "lai";
    data.channelBox = true;
    helper.MakeInputBoolean(data);
    #endif
}

bool CWalterStandinTranslator::ExportMayaShadingGraph()
{
    const MPlug layersAssignation = m_DagNode.findPlug("layersAssignation");
    if (layersAssignation.isNull())
    {
        AiMsgWarning(
                "[mtoa] [%s] Can't find %s.layersAssignation.",
                GetTranslatorName().asChar(),
                m_DagNode.name().asChar());
        return false;
    }

    if (layersAssignation.numElements() <= 0)
    {
        return false;
    }

    for (unsigned i=0; i<layersAssignation.numElements(); i++)
    {
        // Get walterStandin.layersAssignation[i]
        const MPlug currentLayerCompound =
            layersAssignation.elementByPhysicalIndex(i);
        // Get walterStandin.layersAssignation[i].layer
        MPlug layerPlug;
        if (!GetChildMPlug(currentLayerCompound, "layer", layerPlug))
        {
            continue;
        }

        MPlugArray connections;
        if (!layerPlug.connectedTo(connections, true, false) ||
            !connections.length())
        {
            continue;
        }

        // The layer is the first connected node. We consider we have only one
        // connection.
        const MFnDependencyNode layer(connections[0].node());
        const std::string layerName = layer.name().asChar();
        bool mainLayer = layerName == "defaultRenderLayer";

        MPlug shadersPlug;
        if (!GetChildMPlug(
                    currentLayerCompound, "shaderConnections", shadersPlug))
        {
            continue;
        }

        for (unsigned j=0; j<shadersPlug.numElements(); j++)
        {
            // Get walterStandin.layersAssignation[i].shaderConnections[j]
            const MPlug shadersCompound = shadersPlug.elementByPhysicalIndex(j);
            // Get walterStandin.layersAssignation[].shaderConnections[].abcnode
            MPlug abcnodePlug;
            if (!GetChildMPlug(shadersCompound, "abcnode", abcnodePlug))
            {
                continue;
            }

            const MString abcnode = abcnodePlug.asString().asChar();
            if (!abcnode.length())
            {
                continue;
            }

            // Get walterStandin.layersAssignation[].shaderConnections[].shader
            MPlug shaderPlug;
            if (GetChildMPlug(shadersCompound, "shader", shaderPlug))
            {
                ExportConnections(
                        abcnode.asChar(), shaderPlug);
            }

            // The same for displacement
            MPlug dispPlug;
            if (GetChildMPlug(shadersCompound, "displacement", dispPlug))
            {
                ExportConnections(abcnode.asChar(), dispPlug);
            }
        }
    }

    return true;
}

void CWalterStandinTranslator::ExportConnections(
        const char* abcnode, const MPlug& shaderPlug)
{
    MPlugArray connections;
    if (!shaderPlug.connectedTo(connections, true, false) ||
        !connections.length())
    {
        return;
    }

    // Push this connection to the shader to Arnold.
#if WALTER_MTOA_VERSION >= 10400
    ExportConnectedNode(connections[0]);
#else
    ExportNode(connections[0]);
#endif
}

int CWalterStandinTranslator::ComputeWalterVisibility(
        const MFnDependencyNode& node)const
{
    unsigned visibility = AI_RAY_ALL;
    bool exists = false;
    MPlug plug;

    plug = node.findPlug("walterVisibility");

    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            return 0;
        }
    }

    plug = node.findPlug("walterCastsShadows");
    if (!plug.isNull())
    {
        exists = true;
        if(!plug.asBool())
        {
            visibility &= ~AI_RAY_SHADOW;
        }
    }

    plug = node.findPlug("walterPrimaryVisibility");
    MString plugName = plug.name();
    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            visibility &= ~AI_RAY_CAMERA;
        }
    }

    plug = node.findPlug("walterVisibleInReflections");
    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            visibility &= ~RDO_AI_RAY_REFLECTED;
        }
    }

    plug = node.findPlug("walterVisibleInRefractions");
    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            visibility &= ~RDO_AI_RAY_REFRACTED;
        }
    }

    plug = node.findPlug("walterVisibleInDiffuse");
    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            visibility &= ~RDO_AI_RAY_DIFFUSE;
        }
    }

    plug = node.findPlug("walterVisibleInGlossy");
    if (!plug.isNull())
    {
        exists = true;
        if (!plug.asBool())
        {
            visibility &= ~RDO_AI_RAY_GLOSSY;
        }
    }

    if (!exists)
    {
        return -1;
    }

    return visibility;
}

#if WALTER_MTOA_VERSION >= 10400
void CWalterStandinTranslator::ExportFrame(AtNode* node)
#else
void CWalterStandinTranslator::ExportFrame(AtNode* node, unsigned int step)
#endif
{
    // The reference implementation is CDagTranslator::ExportMatrix.
    MTime time = m_DagNode.findPlug("time").asMTime() +
        m_DagNode.findPlug("timeOffset").asMTime();
    float frame = time.as(time.unit());

#if WALTER_MTOA_VERSION >= 10400
    if (!IsExportingMotion())
#else
    if (step == 0)
#endif
    {
        // We are here because it's called from Export.
        if (RequiresMotionData())
        {
#if WALTER_MTOA_VERSION >= 10400
            int step = GetMotionStep();
#endif
            AiNodeDeclare(node, "frame", "constant ARRAY FLOAT");

            AtArray* frames =
                AiArrayAllocate(1, GetNumMotionSteps(), AI_TYPE_FLOAT);
            AiArraySetFlt(frames, step, frame);
            AiNodeSetArray(node, "frame", frames);
        }
        else
        {
            AiNodeDeclare(node, "frame", "constant FLOAT");

            AiNodeSetFlt(node, "frame", frame);
        }
    }
    else if (IsMotionBlurEnabled(MTOA_MBLUR_OBJECT) && RequiresMotionData())
    {
        // We are here because it's called from ExportMotion. Everything should
        // be delared. We need to set specific keys in the Arnold attribute.
        AtArray* frames = AiNodeGetArray(node, "frame");
        if (frames)
        {
#if WALTER_MTOA_VERSION >= 10400
            int step = GetMotionStep();
#endif
            int arraySize =
#if AI_VERSION_ARCH_NUM == 4
                frames->nkeys * frames->nelements;
#else
                AiArrayGetNumKeys(frames) * AiArrayGetNumElements(frames);
#endif
            if (step >= arraySize)

            {
                AiMsgError(
                    "Time steps are not set properly for %s",
                    m_dagPath.partialPathName().asChar());
            }
            else
            {
                AiArraySetFlt(frames, step, frame);
            }
        }
    }
}
