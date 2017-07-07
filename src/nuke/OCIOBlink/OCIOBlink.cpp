#include "DDImage/PlanarIop.h"
#include "DDImage/Knobs.h"
#include "DDImage/NukeWrapper.h"
#include "DDImage/Blink.h"
#include "Blink/Blink.h"

#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;


static const char* const CLASS = "OCIOBlink";
static const char* const HELP = "Example of a simple box blur and gain operator that uses the Blink API.";


namespace {

    std::string OCIOKernelBasis = ""
        "kernel OCIOBlinkKernel : ImageComputationKernel<ePixelWise>"
        "{\n"
        "  Image<eRead, eAccessPoint, eEdgeClamped> src;\n"
        "  Image<eWrite> dst;\n"
        "\n"
        "  local:\n"
        "\n"
        "  void process() {\n"
        "    dst() = src.kComps;\n"
        "    return;\n"
        "    //float lut[] = {1,1,1};\n" // FIXME
        "    //dst() = ocio_blink_func(src(), lut);\n"
        "   }\n"
        "};\n";
}


class OCIOBlink : public DD::Image::PlanarIop
{
protected:
    // Reference to the GPU device to process on.
    Blink::ComputeDevice _gpuDevice;

    // Whether to process on the GPU, if available
    bool _useGPUIfAvailable;
    bool _useBlink;

    // The amount of gain to apply.
    float _gain;

    int modeindex;
    OCIO::ConstProcessorRcPtr processor;

    // This holds the ProgramSource for the gain kernel.
    Blink::ProgramSource _blinkProgram;

public:
    static const char* modes[];

    OCIOBlink(Node* node) 
        : PlanarIop(node)
        , _gpuDevice(Blink::ComputeDevice::CurrentGPUDevice())
        , _useGPUIfAvailable(true)
        , _useBlink(true)
        , _blinkProgram("")
    {
        _gain = 2.0f;
    }

    void knobs(DD::Image::Knob_Callback f);
    void _validate(bool);
    void getRequests(const DD::Image::Box& box, const DD::Image::ChannelSet& channels, int count, DD::Image::RequestOutput &reqData) const;

    // Whether to process in stripes or full-frame.
    virtual bool useStripes() const;
    // Set the stripe height to use for processing.
    virtual size_t stripeHeight() const;

    void renderStripe(DD::Image::ImagePlane& outputPlane);

    const char* Class() const { return CLASS; }
    const char* node_help() const { return HELP; }
    static const Iop::Description description;
};



const char* OCIOBlink::modes[] = {
    "log to lin", "lin to log", 0
};


void OCIOBlink::knobs(DD::Image::Knob_Callback f)
{
    // Add GPU knobs
    Newline(f, "Local GPU: ");
    const bool hasGPU = _gpuDevice.available();
    std::string gpuName = hasGPU ? _gpuDevice.name() : "Not available";
    Named_Text_knob(f, "gpuName", gpuName.c_str());
    Newline(f);
    Bool_knob(f, &_useGPUIfAvailable, "use_gpu", "Use GPU if available");
    Divider(f);


    Bool_knob(f, &_useBlink, "use_blink", "Use Blink!");
    Divider(f);

    // Add a parameter for the gain amount.
    Float_knob(f, &_gain, DD::Image::IRange(0, 10), "gain");
    Tooltip(f, "The amount of gain to apply.");


    // Log Convert knobs
    Enumeration_knob(f, &modeindex, modes, "operation", "operation");
    DD::Image::SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
}

void OCIOBlink::_validate(bool for_real)
{
    // Copy bbox channels etc from input0, which will validate it.
    copy_info(); 

    try
    {
        OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
        
        const char * src = 0;
        const char * dst = 0;
        
        if(modeindex == 0)
        {
            src = OCIO::ROLE_COMPOSITING_LOG;
            dst = OCIO::ROLE_SCENE_LINEAR;
        }
        else
        {
            src = OCIO::ROLE_SCENE_LINEAR;
            dst = OCIO::ROLE_COMPOSITING_LOG;
        }
        
        processor = config->getProcessor(src, dst);
    }
    catch(OCIO::Exception &e)
    {
        error(e.what());
        return;
    }


    if(processor->isNoOp())
    {
        set_out_channels(DD::Image::Mask_None); // prevents engine() from being called
    } else {    
        set_out_channels(DD::Image::Mask_All);
    }


    int LUT3D_EDGE_SIZE = 2;

    // Get Blink kernel text
    OCIO::GpuShaderDesc desc;
    desc.setLanguage(OCIO::GPU_LANGUAGE_BLINK);
    desc.setFunctionName("ocio_blink_func");
    desc.setLut3DEdgeLen(LUT3D_EDGE_SIZE);

    std::string a = processor->getGpuShaderText(desc);

    std::ostringstream os;
    os << a << "\n\n";
    os << "// Staticly defined:\n";
    os << OCIOKernelBasis << "\n";

    std::cerr << os.str() << "\n";

    _blinkProgram = Blink::ProgramSource(os.str());

    // Get 3D LUT
    int num3Dentries = 3*LUT3D_EDGE_SIZE*LUT3D_EDGE_SIZE*LUT3D_EDGE_SIZE;
    std::vector<float> lut3d;
    lut3d.resize(num3Dentries);
    memset(&lut3d[0], 0, sizeof(float)*num3Dentries);

    // Preview LUT
    processor->getGpuLut3D(&lut3d[0], desc);
    for(int i=0; i<num3Dentries; i++){
        //std::cerr << lut3d[num3Dentries] << " ";
    }
    //std::cerr << "\n";

}

void  OCIOBlink::getRequests(const DD::Image::Box& box, const DD::Image::ChannelSet& channels, int count, DD::Image::RequestOutput &reqData) const
{
    reqData.request(&input0(), box, channels, count);
}

// Note that this is copied by others (OCIODisplay)
void OCIOBlink::in_channels(int /* n unused */, DD::Image::ChannelSet& mask) const
{
    DD::Image::ChannelSet done;
    foreach(c, mask)
    {
        if (DD::Image::colourIndex(c) < 3 && !(done & c))
        {
            done.addBrothers(c, 3);
        }
    }
    mask += done;
}

void OCIOBlink::renderStripe(DD::Image::ImagePlane& outputPlane)
{
    // CPU Path
    if(!_useBlink)
    {
        input0().fetchPlane(outputPlane);
        outputPlane.makeUnique();
        outputPlane.makeWritable();
        DD::Image::Box box = outputPlane.bounds();

        OCIO::PackedImageDesc img(
            outputPlane.writable(),
            outputPlane.bounds().w(), outputPlane.bounds().h(),
            outputPlane.nComps());

        processor->apply(img);

        return;
    } // end CPU path


    // GPU path:

    // Make an ImagePlaneDescriptor that describes how the inputs should be stored.
    DD::Image::ImagePlaneDescriptor inputDescriptor(
        outputPlane.bounds(), // the bounds of the input we want to fetch
        outputPlane.packed(), // input data should be packed in the same way as the output plane
        outputPlane.channels(), // input data should have the same channels as the output plane
        outputPlane.nComps());  // input data should have the same number of components as the output plane

    // Make an ImagePlane that satisfies this description.
    DD::Image::ImagePlane inputPlane(inputDescriptor);

    // Fetch the data from input0 into our ImagePlane.
    input0().fetchPlane(inputPlane);

    // This function must be called on the output plane before writing to it.
    outputPlane.makeWritable();


    // Wrap the input and output planes as Blink images. The underlying data stays the same. 
    Blink::Image outputPlaneAsImage;
    Blink::Image inputPlaneAsImage;
    bool success = (DD::Image::Blink::ImagePlaneAsBlinkImage(outputPlane, outputPlaneAsImage) &&
                    DD::Image::Blink::ImagePlaneAsBlinkImage(inputPlane, inputPlaneAsImage));
  
    // Check the fetch succeeded.
    if (!success) {
        error("Unable to fetch Blink image for image plane.");
        return;
    }

    // Has the user requested GPU processing, and is the GPU available for processing on?
    bool usingGPU = _useGPUIfAvailable && _gpuDevice.available();

    // Get a reference to the ComputeDevice to do our processing on. 
    Blink::ComputeDevice computeDevice = usingGPU ? _gpuDevice : Blink::ComputeDevice::CurrentCPUDevice();

    // Distribute our input image from the device used by NUKE to our ComputeDevice.
    Blink::Image inputImageOnComputeDevice = inputPlaneAsImage.distributeTo(computeDevice);

    // This will bind the compute device to the calling thread. This bind should always be performed before
    // beginning any image processing with Blink.
    Blink::ComputeDeviceBinder binder(computeDevice);

    // Make a vector containing the images we want to run the first kernel over.
    std::vector<Blink::Image> images;

    // If we are on the GPU, we need to make an output image, otherwise we can just use the one from NUKE.
    Blink::Image outputImage = usingGPU ? outputPlaneAsImage.makeLike(_gpuDevice) : outputPlaneAsImage;

    // The kernel requires input and put output images
    images.clear();
    images.push_back(inputImageOnComputeDevice);
    images.push_back(outputImage);

    // Make a Blink::Kernel from the source in _gainProgram to do the gain.
    try{
        Blink::Kernel kernel(_blinkProgram, 
                             computeDevice, 
                             images,
                             kBlinkCodegenDefault);
        //kernel.setParamValue("Gain", _gain);

        // Run the gain kernel over the output image.
        kernel.iterate();
        
        // If we're using the GPU, copy the result back to NUKE's output plane.
        if (usingGPU) {
            outputPlaneAsImage.copyFrom(outputImage);
        }

    } catch(Blink::ParseException & e){
        std::ostringstream os;
        os << "Error parsing Blink kernel: " << e.parseError() << " at line " << e.lineNumber();
        std::cerr << os.str() << "\n"; // TODO: Dump kernel source
        error(os.str().c_str());
        return;

    } catch(Blink::Exception & e){
        std::cerr << "Blink kernel error: " << e.userMessage() << "\n";
        error(e.userMessage().c_str());
        return;
    }
}

bool OCIOBlink::useStripes() const
{
    return true;
}

size_t OCIOBlink::stripeHeight() const
{
    return 1;
}

static DD::Image::Iop* OCIOBlinkCreate(Node* node)
{
    DD::Image::NukeWrapper *op = new DD::Image::NukeWrapper(new OCIOBlink(node));
    op->channels(DD::Image::Mask_RGB);
    return op;
}

const DD::Image::Iop::Description OCIOBlink::description(CLASS, "Filter/OCIOBlink",
                                                         OCIOBlinkCreate);
