//
//      $Id$
//
//***********************************************************************
//                                                                       *
//                            Copyright (C)  2005                        *
//            University Corporation for Atmospheric Research            *
//                            All Rights Reserved                        *
//                                                                       *
//***********************************************************************/
//
//      File:		raw2vdf.cpp
//
//      Author:         John Clyne
//                      National Center for Atmospheric Research
//                      PO 3000, Boulder, Colorado
//
//      Date:           Tue Jun 14 15:01:13 MDT 2005
//
//      Description:	Read a file containing a raw data volume. Translate
//			and append the volume to an existing
//			Vapor Data Collection
//
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cerrno>

#include <vapor/CFuncs.h>
#include <vapor/OptionParser.h>
#include <vapor/MetadataVDC.h>
#include <vapor/WaveletBlock3DBufWriter.h>
#include <vapor/WaveletBlock3DRegionWriter.h>
#include <vapor/WaveCodecIO.h>
#include <vapor/CFuncs.h>
#ifdef WIN32
#include "windows.h"
#endif

using namespace VetsUtil;
using namespace VAPoR;


//
//	Command line argument stuff
//
struct opt_t {
	int	ts;
	char *varname;
	int level;
	int lod;
	int nthreads;
	int skip;
	float min;
	float max;
	OptionParser::Boolean_T eightBit;
	OptionParser::Boolean_T	help;
	OptionParser::Boolean_T	debug;
	OptionParser::Boolean_T	quiet;
	OptionParser::Boolean_T	swapbytes;
	OptionParser::Boolean_T	dbl;
	OptionParser::IntRange_T xregion;
	OptionParser::IntRange_T yregion;
	OptionParser::IntRange_T zregion;
	int staggeredDim;
};

static opt_t opt;

static OptionParser::OptDescRec_T	set_opts[] = {
	{"ts",		1, 	"0","Timestep of data file starting from 0"},
	{"varname",	1, 	"var1",	"Name of variable"},
	{"level",	1, 	"-1",	"Refinement levels saved. 0 => coarsest, 1 => "
		"next refinement, etc. -1 => all levels defined by the .vdf file"},
	{"lod",	1, 	"-1",	"Compression levels saved. 0 => coarsest, 1 => "
		"next refinement, etc. -1 => all levels defined by the .vdf file"},
	{"nthreads",1, 	"0",	"Number of execution threads (0 => # processors)"},
	{"skip",1, 	"0",	"Seek past n bytes before reading from input"},
	{"help",	0,	"",	"Print this message and exit"},
	{"debug",	0,	"",	"Enable debugging"},
	{"quiet",	0,	"",	"Operate quietly"},
	{"swapbytes",	0,	"",	"Swap bytes in raw data as they are read from disk"},
	{"dbl",	0,	"",	"Input data are 64-bit floats"},
	{"eightBit", 0, "", "Input data are 8 bit integers"},
	{"min", 1, "0.0", "Minimum value of input variable (ignored unless -eightBit is declared"},
	{"max", 1, "0.0", "Maximum value of input variable (ignored unless -eightBit is declared"},
	{"xregion", 1, "-1:-1", "X dimension subregion bounds (min:max)"},
	{"yregion", 1, "-1:-1", "Y dimension subregion bounds (min:max)"},
	{"zregion", 1, "-1:-1", "Z dimension subregion bounds (min:max)"},
	{"stagdim", 1, "0", "1, 2, or 3 indicate staggered in x, y, or z"},
	{NULL}
};


static OptionParser::Option_T	get_options[] = {
	{"ts", VetsUtil::CvtToInt, &opt.ts, sizeof(opt.ts)},
	{"varname", VetsUtil::CvtToString, &opt.varname, sizeof(opt.varname)},
	{"level", VetsUtil::CvtToInt, &opt.level, sizeof(opt.level)},
	{"lod", VetsUtil::CvtToInt, &opt.lod, sizeof(opt.lod)},
	{"nthreads", VetsUtil::CvtToInt, &opt.nthreads, sizeof(opt.nthreads)},
	{"skip", VetsUtil::CvtToInt, &opt.skip, sizeof(opt.skip)},
	{"help", VetsUtil::CvtToBoolean, &opt.help, sizeof(opt.help)},
	{"debug", VetsUtil::CvtToBoolean, &opt.debug, sizeof(opt.debug)},
	{"quiet", VetsUtil::CvtToBoolean, &opt.quiet, sizeof(opt.quiet)},
	{"swapbytes", VetsUtil::CvtToBoolean, &opt.swapbytes, sizeof(opt.swapbytes)},
	{"dbl", VetsUtil::CvtToBoolean, &opt.dbl, sizeof(opt.dbl)},
	{"eightBit", VetsUtil::CvtToBoolean, &opt.eightBit, sizeof(opt.eightBit)},
	{"min", VetsUtil::CvtToFloat, &opt.min, sizeof(opt.min)},
	{"max", VetsUtil::CvtToFloat, &opt.max, sizeof(opt.max)},
	{"xregion", VetsUtil::CvtToIntRange, &opt.xregion, sizeof(opt.xregion)},
	{"yregion", VetsUtil::CvtToIntRange, &opt.yregion, sizeof(opt.yregion)},
	{"zregion", VetsUtil::CvtToIntRange, &opt.zregion, sizeof(opt.zregion)},
	{"stagdim", VetsUtil::CvtToInt, &opt.staggeredDim, sizeof(opt.staggeredDim)},
	{NULL}
};

static const char	*ProgName;

	
void    swapbytes(
	void *vptr,
	int size,
	int	n
) {
	unsigned char   *ucptr = (unsigned char *) vptr;
	unsigned char   uc;
	int             i,j;

	for (j=0; j<n; j++) {
		for (i=0; i<size/2; i++) {
			uc = ucptr[i];
			ucptr[i] = ucptr[size-i-1];
			ucptr[size-i-1] = uc;
		}
		ucptr += size;
	}
}

// Add dummy variable to determine template type
// Delete float/double shuffling and replace with memcpy
// special casing for element_sz gets replaced with sizeof(T)
template <class T>
int read_next_slice(
	T type,
	const VDFIOBase *vdfio,
	const size_t dim[2],
	FILE	*fp, 
	float *slice,
	float *read_timer
) {

	static T *buffer = NULL;
	static float *writeBuffer = NULL;
	static bool first = true;

	double t0;
 
	// Allocate a buffer large enough to hold one slice of data,
	// plus one if staggered.
	//
	int element_sz = sizeof(T);

	//dimx and dimy are the size of the input data:
	size_t dimx,dimy;
	dimx = (opt.staggeredDim == 1) ? dim[0]+1 : dim[0];
	dimy = (opt.staggeredDim == 2) ? dim[1]+1 : dim[1];
	size_t read_sz = dimx*dimy;
	T *readbuffer;
	float *writePtr;

	if (first) {
		*read_timer = 0;

		// First read for z staggered data we read two slices
		//
		if (opt.staggeredDim == 3) read_sz *= 2;

		buffer = new T [read_sz];
		readbuffer = buffer;
		
		writeBuffer = new float[read_sz];
		writePtr = writeBuffer;

		first = false;
	}
	else {
		//
		// If z is staggered we read 2nd slice into bottom of buffer,
		// just past where previous slice is stored. N.B. dimensions
		// of previous slice are dim[0]*dim[1]*sizeof(*slice)
		//
		if (opt.staggeredDim == 3) {
			readbuffer = buffer + dim[0]*dim[1];
			writePtr = writeBuffer + dim[0]*dim[1];
		}
		else {
			readbuffer = buffer;
			writePtr = writeBuffer;
		}
	}

	t0 = GetTime();

	int rc = fread(readbuffer, element_sz, read_sz, fp);
	if (rc != read_sz) {
		if (rc<0) { 
			MyBase::SetErrMsg("Error reading input file : %M");
		} else {
			MyBase::SetErrMsg("Short read on input file");
		}
		return(-1);
	}

	*read_timer += GetTime() - t0;

	// Swap bytes in place if needed
	//
	if (opt.swapbytes) {
		swapbytes(readbuffer, element_sz, read_sz); 
	}

	// Apply scale factors to eight bit data 
	// 
	if (opt.eightBit) {
		T *readPtr = (T*) buffer;
		float scale = ((float)opt.max-(float)opt.min)/256.f;
		for(int i=0; i<read_sz; i++) {
			*writePtr = opt.min + (*readPtr)*scale;
			writePtr++;
			readPtr++;
		}
	}
	// Otherwise just copy values straight into writeBuffer
	//
	else std::copy(readbuffer, readbuffer+read_sz, writePtr);

	// Apply averageing for staggered dimensions
	//
	float* fslice = writePtr;
	if (opt.staggeredDim == 1){
		size_t inposn = 0;
		//Loop over output positions:
		for (int j = 0; j<dim[1]; j++){
			for (int i = 0; i< dim[0]; i++){
				fslice[i+dim[0]*j] = 
					0.5*(fslice[inposn]+fslice[inposn+1]);
				inposn++;
			}
			//At end of row, skip one position:
			inposn++;
		}
		//at the end, the inposn should be one past the end of the data:
		assert(inposn == dimx*dimy);
	}

	//
	// If staggered in y, average each row, ignore the
	// last row
	//
	else if (opt.staggeredDim == 2) {
		for (int j = 0; j<dim[1]; j++){
		for (int i = 0; i<dim[0]; i++){
			fslice[i+dim[0]*j] = (fslice[i+dim[0]*j]+fslice[i+dim[0]*(j+1)])*0.5;
		}
		}
	}
	else if (opt.staggeredDim == 3) {
		//Average old and new slices:
		//
		float *old_fslice = writeBuffer;
		fslice = old_fslice + dim[0]*dim[1];
		float *new_fslice = fslice;

		for (int i = 0; i< dim[0]*dim[1]; i++){
			float v = *new_fslice;
			*new_fslice = 0.5*(*new_fslice + *old_fslice);
			*old_fslice = v;

			old_fslice++;
			new_fslice++;
		}
	}

	memcpy(slice, fslice, dim[0]*dim[1]*sizeof(*slice));

	return 0;
}

int	process_volume(
	VDFIOBase *vdfio,
	FILE *fp,
	Metadata::VarType_T vtype,
	float *read_timer,
	float *write_timer,
	float *xform_timer
) {

	const size_t *dim = vdfio->GetDimension();

	size_t dim3d[3];
	switch (vtype) {
	case Metadata::VAR2D_XY:
		dim3d[0] = dim[0];
		dim3d[1] = dim[1];
		dim3d[2] = 1;
	break;
	case Metadata::VAR2D_XZ:
		dim3d[0] = dim[0];
		dim3d[1] = dim[2];
		dim3d[2] = 1;
	break;
	case Metadata::VAR2D_YZ:
		dim3d[0] = dim[1];
		dim3d[1] = dim[2];
		dim3d[2] = 1;
	break;
	case Metadata::VAR3D:
		dim3d[0] = dim[0];
		dim3d[1] = dim[1];
		dim3d[2] = dim[2];
	break;
	default:
	break;

	}

	float *slice = new float[dim3d[0]*dim3d[1]];

	int rc;
	rc = vdfio->OpenVariableWrite(opt.ts,opt.varname, opt.level, opt.lod);
	if (rc<0) {
		MyBase::SetErrMsg(
			"Failed to open variable \"%s\" for writing", opt.varname
		);
		return 1;
	}

	for (size_t z=0; z<dim3d[2]; z++) {

		if (z%10== 0 && ! opt.quiet) {
			cout << "Reading slice # " << z << endl;
		}

		float fl = 0.0;
		unsigned char uc = 0;
		double db = 0;
		if (opt.eightBit) rc = read_next_slice( uc, vdfio, dim3d, fp, slice, read_timer);
		else if (opt.dbl) rc = read_next_slice(db, vdfio, dim3d, fp, slice, read_timer);
		else rc = read_next_slice(fl, vdfio, dim3d, fp, slice, read_timer);
		if (rc<0) return 1;

		rc = vdfio->WriteSlice(slice);
		if (rc<0) {
			MyBase::SetErrMsg(
				"Failed to write slice # %d of variable \"%s\"", z, opt.varname
			);
			return 1;
		}
	}

	rc = vdfio->CloseVariable();
	if (rc<0) {
		MyBase::SetErrMsg("Error closing output file"); 
		return 1;
	}

	*write_timer = vdfio->GetWriteTimer();
	*xform_timer = vdfio->GetXFormTimer();

	delete [] slice;
	
	return 0;
}


float *read_region(
	VDFIOBase *vdfio,
	FILE	*fp, 
	Metadata::VarType_T vtype,
	size_t min[3],
	size_t max[3],
	float *read_timer
) {

	// Get the dimensions of the volume
	//
	const size_t *dim = vdfio->GetDimension();

	size_t dim3d[3];

	switch (vtype) {
	case Metadata::VAR2D_XY:
		min[0] = opt.xregion.min == (size_t) -1 ? 0 : opt.xregion.min;
		max[0] = opt.xregion.max == (size_t) -1 ? dim[0] - 1 : opt.xregion.max;
		min[1] = opt.yregion.min == (size_t) -1 ? 0 : opt.yregion.min;
		max[1] = opt.yregion.max == (size_t) -1 ? dim[1] - 1 : opt.yregion.max;
		min[2] = max[2] = 0;
	break;

	case Metadata::VAR2D_XZ:
		min[0] = opt.xregion.min == (size_t) -1 ? 0 : opt.xregion.min;
		max[0] = opt.xregion.max == (size_t) -1 ? dim[0] - 1 : opt.xregion.max;
		min[2] = opt.zregion.min == (size_t) -1 ? 0 : opt.zregion.min;
		max[2] = opt.zregion.max == (size_t) -1 ? dim[2] - 1 : opt.zregion.max;
		min[1] = max[1] = 0;
	break;
	case Metadata::VAR2D_YZ:
		min[1] = opt.yregion.min == (size_t) -1 ? 0 : opt.yregion.min;
		max[1] = opt.yregion.max == (size_t) -1 ? dim[1] - 1 : opt.yregion.max;
		min[2] = opt.zregion.min == (size_t) -1 ? 0 : opt.zregion.min;
		max[2] = opt.zregion.max == (size_t) -1 ? dim[2] - 1 : opt.zregion.max;
		min[0] = max[0] = 0;
	break;
	case Metadata::VAR3D:
		min[0] = opt.xregion.min == (size_t) -1 ? 0 : opt.xregion.min;
		max[0] = opt.xregion.max == (size_t) -1 ? dim[0] - 1 : opt.xregion.max;
		min[1] = opt.yregion.min == (size_t) -1 ? 0 : opt.yregion.min;
		max[1] = opt.yregion.max == (size_t) -1 ? dim[1] - 1 : opt.yregion.max;
		min[2] = opt.zregion.min == (size_t) -1 ? 0 : opt.zregion.min;
		max[2] = opt.zregion.max == (size_t) -1 ? dim[2] - 1 : opt.zregion.max;
	break;
	default:
	break;

	}
	
	for(int i=0; i<3; i++) {
		dim3d[i] = max[i]-min[i]+1;
	}
	if (vtype != Metadata::VAR3D) dim3d[2] = 1;

	// Allocate a buffer large enough to hold entire subregion
	//
	size_t size = dim3d[0]*dim3d[1]*dim3d[2];

	float *region = new float[size];

	//
	// Translate the volume one slice at a time
	//
	float *slice = region;
	int rc;
	for(int z=0; z<dim3d[2]; z++) {

		if (z%10== 0 && ! opt.quiet) {
			cout << "Reading slice # " << z << endl;
		}
		float fl=0;
		unsigned char uc=0;
		double db=0;
		if (opt.eightBit) rc = read_next_slice(uc, vdfio, dim3d, fp, slice, read_timer);
		else if (opt.dbl) rc = read_next_slice(db, vdfio, dim3d, fp, slice, read_timer);
		else rc = read_next_slice(fl, vdfio, dim3d, fp, slice, read_timer);
		if (rc<0) return NULL;

		slice += dim3d[0]*dim3d[1];
	}

	return(region);
}


int	process_region(
	VDFIOBase *vdfio,
	FILE	*fp, 
	Metadata::VarType_T vtype,
	float *read_timer,
	float *write_timer,
	float *xform_timer
) {

	int rc;
	rc = vdfio->OpenVariableWrite(opt.ts, opt.varname, opt.level, opt.lod);
	if (rc<0) {
		MyBase::SetErrMsg(
			"Failed to open variable \"%s\" for writing", opt.varname
		);
		return 1;
	}


	float *buf = NULL;
	size_t min[3], max[3];

	buf = read_region(vdfio, fp, vtype, min, max, read_timer);
	if (!buf) return 1;

	vdfio->WriteRegion((float *) buf, min, max);
	if (vdfio->GetErrCode() != 0) {
		MyBase::SetErrMsg(
			"Failed to write region of variable \"%s\"", opt.varname
		); 
		return 1;
	}

	delete [] buf;

	rc = vdfio->CloseVariable();
	if (rc<0) {
		MyBase::SetErrMsg("Error closing output file"); 
		return 1;
	}

	*write_timer = vdfio->GetWriteTimer();
	*xform_timer = vdfio->GetXFormTimer();
	
	return 0;
}


void ErrMsgCBHandler(const char *msg, int) {
	cerr << ProgName << " : " << msg << endl;
}


extern "C" int raw2vdf(int argc, char **argv) {

	OptionParser op;
	FILE	*fp;
	const char	*metafile;
	const char	*datafile;

	float	timer = 0.0;
	float	read_timer = 0.0;
	float	write_timer = 0.0;
	float	xform_timer = 0.0;
	string	s;

	MyBase::SetErrMsgCB(ErrMsgCBHandler);

	//
	// Parse command line arguments
	//
	ProgName = Basename(argv[0]);

	if (op.AppendOptions(set_opts) < 0) {
		return 1;
	}

	if (op.ParseOptions(&argc, argv, get_options) < 0) {
		return 1;
	}

	if (opt.help) {
		cerr << "Usage: " << ProgName << " [options] vdfFile rawFile" << endl;
		op.PrintOptionHelp(stderr);
		return 1;
	}
	
	if (argc != 3) {
		cerr << "Usage: " << ProgName << " [options] vdfFile rawFile" << endl;
		op.PrintOptionHelp(stderr);
		return 1;
	}


	metafile = argv[1];	// Path to a vdf file
	datafile = argv[2];	// Path to raw data file 

    if (opt.debug) MyBase::SetDiagMsgFilePtr(stderr);
    
    MyBase::EnableErrMsg(true);
    MyBase::SetErrMsgFilePtr(stderr);

	WaveletBlockIOBase	*wbwriter3D = NULL;
	WaveCodecIO	*wcwriter = NULL;
	VDFIOBase *vdfio = NULL;

	size_t min[3] = {opt.xregion.min, opt.yregion.min, opt.zregion.min};
	size_t max[3] = {opt.xregion.max, opt.yregion.max, opt.zregion.max};

	// Determine if variable is 3D
	//
	MetadataVDC metadata (metafile);
	if (MetadataVDC::GetErrCode() != 0) {
		MyBase::SetErrMsg("Error processing metafile \"%s\"", metafile);
		return 1;
	}
	Metadata::VarType_T vtype = metadata.GetVarType(opt.varname);
	if (vtype == Metadata::VARUNKNOWN) {
		MyBase::SetErrMsg("Unknown variable \"%s\"", opt.varname);
		return 1;
	}

	
	bool vdc1 = (metadata.GetVDCType() == 1);
	if (vdc1) {

		// Create an appropriate WaveletBlock writer. 
		//
		if (min[0] == min[1] && min[1] == min[2] && min[2] == max[0] &&
			max[0] == max[1]  && max[1] == max[2] && max[2] == (size_t) -1 &&
			vtype == Metadata::VAR3D) {

			wbwriter3D = new WaveletBlock3DBufWriter(metadata);
		}
		else {
			wbwriter3D = new WaveletBlock3DRegionWriter(metadata);
		}
		vdfio = wbwriter3D;
	} 
	else {
		wcwriter = new WaveCodecIO(metadata, opt.nthreads);
		vdfio = wcwriter;
	}
	if (vdfio->GetErrCode() != 0) {
		return 1;
	}

	fp = FOPEN64(datafile, "rb");
	if (! fp) {
		MyBase::SetErrMsg("Could not open file \"%s\" : %M", datafile);
		return 1;
	}

	if (opt.skip) {
		int rc = fseek(fp, opt.skip, SEEK_SET);
		if (rc<0) {	
			MyBase::SetErrMsg("Could not seek file \"%s\" : %M", datafile);
			return 1;
		}
	}


	double t0 = GetTime();

	if (min[0] == min[1] && min[1] == min[2] && min[2] == max[0] &&
		max[0] == max[1]  && max[1] == max[2] && max[2] == (size_t) -1 &&
		vtype == Metadata::VAR3D) {

		int err = process_volume(
			vdfio, fp, vtype, &read_timer,
			&write_timer, &xform_timer
		);
		if (err) return err;
	}
	else {
		int err = process_region(
			vdfio, fp, vtype, 
			&read_timer, &write_timer, &xform_timer
		);
		if (err) return err;
	}


	timer = GetTime() - t0;

	if (! opt.quiet) {
		const float *range = vdfio->GetDataRange();

		fprintf(stdout, "read time : %f\n", read_timer);
		fprintf(stdout, "write time : %f\n", write_timer);
		fprintf(stdout, "transform time : %f\n", xform_timer);
		fprintf(stdout, "total transform time : %f\n", timer);
		fprintf(stdout, "min and max values of data output: %g, %g\n",range[0], range[1]);
	}

	return 0;
}

