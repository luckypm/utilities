#include <stdio.h>
#include <getopt.h>
#include <expat.h>
#include <iostream>
#include <math.h>
#include <algorithm>  // std::min

#define EIGEN_NO_DEBUG
//#define EIGEN_DONT_VECTORIZE
//#define EIGEN_DONT_PARALLELIZE  // don't use openmp

#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/SVD>

using namespace Eigen;

#define QUATOSTOOL_VERSION "150304.0"  // yymmdd.build

#define MAX_DEPTH	16
#define DEG_TO_RAD (M_PI / 180.0f)

#define NUM_PORTS	16

#define DEFAULT_MASS_ARM	80
#define DEFAULT_MASS_MOTOR	100
#define DEFAULT_MASS_ESC 	20
#define DEFAULT_DIST_MOTOR	0.25
#define DEFAULT_DIST_ESC	0.1

enum {
	ELEMENT_QUATOS_CONFIGURATION = 0,
	ELEMENT_CRAFT,
	ELEMENT_PORTS,
	ELEMENT_PORT,
	ELEMENT_DISTANCE,
	ELEMENT_MASS,
	ELEMENT_MOTOR,
	ELEMENT_ARM,
	ELEMENT_ESC,
	ELEMENT_CUBE,
	ELEMENT_GEOMETRY,
	ELEMENT_NUM
};

const char *elementNames[] = {
	"quatos_configuration",
	"craft",
	"ports",
	"port",
	"distance",
	"mass",
	"motor",
	"arm",
	"esc",
	"cube",
	"geometry"
};

enum {
	CONFIG_QUAD_PLUS = 0,
	CONFIG_QUAD_X,
	CONFIG_HEX_PLUS,
	CONFIG_HEX_X,
	CONFIG_OCTO_PLUS,
	CONFIG_OCTO_X,
	CONFIG_CUSTOM,
	CONFIG_NUM
};

const char *configTypes[] = {
	"quad_plus",
	"quad_x",
	"hex_plus",
	"hex_x",
	"octo_plus",
	"octo_x",
	"custom"
};

// default config IDs for predefined frame types (from mot_mixes.xml)
const int configIds[] = {
	4,
	5,
	10,
	11,
	30,
	31,
	0
};

typedef struct {
	int elementIds[MAX_DEPTH];
	int level;
	int validCraft;
	int n;
	char value[256];
	int valueLen;
} parseContext_t;

typedef struct {
	int validCraft;
	char craftId[256];
	int craftType;
	int n;				// number of motors
	int configId;
	double totalMass;
	Vector3d offsetCG;
	MatrixXd ports;
	MatrixXd propDir;
	VectorXd frameX;
	VectorXd frameY;
	MatrixXd motorX;
	MatrixXd motorY;
	double massEsc;
	VectorXd massEscs;
	double massMot;
	VectorXd massMots;
	double massArm;
	VectorXd massArms;
	VectorXd massObjects;
	double distMot;
	double distEsc;
	int objectsCount;
	MatrixXd objectsDim;
	MatrixXd objectsOffset;
	MatrixXd PITCH, ROLL, YAW, THROT;
	MatrixXd PD, M, Mt, PID;
	Matrix3d J;
} quatosData_t;

quatosData_t quatosData;
int outputPID;
int outputMIXfile;
int outputDebug;
FILE *outFP;

template<typename _Matrix_Type_>
void displayMatrix(const char *name, _Matrix_Type_ &m) {
	int i, j;

	fprintf(outFP, "%s = [\n", name);
	for (i = 0; i < m.rows(); i++) {
		fprintf(outFP, "\t");
		for (j = 0; j < m.cols(); j++)
			fprintf(outFP, "%+12.7f  ", m(i, j));
		fprintf(outFP, "\n");
	}
	fprintf(outFP, "];\n");
}

template<typename _Matrix_Type_>
void pseudoInverse(const _Matrix_Type_ &a, _Matrix_Type_ &result, double epsilon = std::numeric_limits<typename _Matrix_Type_::Scalar>::epsilon()) {
	Eigen::JacobiSVD<_Matrix_Type_> svd = a.jacobiSvd(Eigen::ComputeThinU |Eigen::ComputeThinV);

	typename _Matrix_Type_::Scalar tolerance =
		epsilon * std::max(a.cols(), a.rows()) * svd.singularValues().array().abs().maxCoeff();

	result = svd.matrixV() * _Matrix_Type_( (svd.singularValues().array().abs() > tolerance).select(svd.singularValues().
	array().inverse(), 0) ).asDiagonal() * svd.matrixU().adjoint();
}

int quatosToolFindPort(int port) {
	int i;

	for (i = 0; i < quatosData.ports.size(); i++)
		if (quatosData.ports(i) == port)
			return i;

	return -1;
}

int quatosToolConfigTypeByName(const XML_Char *name) {
	int i;

	for (i = 0; i < CONFIG_NUM; i++)
		if (!strncasecmp(name, configTypes[i], strlen(configTypes[i])+1))
			return i;

	return -1;
}

int quatosToolElementIdByName(const XML_Char *name) {
	int i;

	for (i = 0; i < ELEMENT_NUM; i++)
		if (!strncasecmp(name, elementNames[i], strlen(elementNames[i])+1))
			return i;

	return -1;
}

const XML_Char *quatosToolFindAttr(const XML_Char **atts, const char *name) {
	const XML_Char *value = NULL;

	while (*atts) {
		if (!strncasecmp(name, *atts, strlen(name))) {
			value = (const XML_Char *)*(++atts);
			break;
		}
		atts += 2;
	}

	return value;
}

void parse(XML_Parser parser, char c, int isFinal) {
	if (XML_STATUS_OK == XML_Parse(parser, &c, isFinal ^ 1, isFinal))
		return;

	fprintf(stderr, "quatosTool: parsing XML failed at line %lu, pos %lu: %s\n",
		(unsigned long)XML_GetCurrentLineNumber(parser),
		(unsigned long)XML_GetCurrentColumnNumber(parser),
		XML_ErrorString(XML_GetErrorCode(parser)) );

	exit(1);
}

void resetCraft(parseContext_t *context) {
	if (!quatosData.n) {
		int n = 0;
		switch (quatosData.craftType) {
			case CONFIG_QUAD_PLUS:
			case CONFIG_QUAD_X:
				n = 4;
				break;
			case CONFIG_HEX_PLUS:
				case CONFIG_HEX_X:
				n = 6;
				break;
				case CONFIG_OCTO_X:
				case CONFIG_OCTO_PLUS:
					n = 8;
				break;
			default:
				context->validCraft = 0;
				break;
		}
		quatosData.n = n;
	}

	quatosData.ports.setZero(1, quatosData.n);
	quatosData.propDir.setZero(1, quatosData.n);
	quatosData.massMots.setZero(quatosData.n);
	quatosData.massEscs.setZero(quatosData.n);
	quatosData.massArms.setZero(quatosData.n);
	quatosData.frameX.resize(quatosData.n);
	quatosData.frameY.resize(quatosData.n);

	quatosData.massEsc = DEFAULT_MASS_ESC;
	quatosData.massMot = DEFAULT_MASS_MOTOR;
	quatosData.massArm = DEFAULT_MASS_ARM;
	quatosData.distEsc = DEFAULT_DIST_ESC;
	quatosData.distMot = DEFAULT_DIST_MOTOR;

	quatosData.validCraft = 1;

}

void parseCube(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;
	
	quatosData.massObjects.conservativeResize(context->n+1);
	quatosData.objectsDim.conservativeResize(context->n+1, 3);
	quatosData.objectsOffset.conservativeResize(context->n+1, 3);

	att = quatosToolFindAttr(atts, "dimx");
	if (att)
		quatosData.objectsDim(context->n, 0) = atof(att);
	att = quatosToolFindAttr(atts, "dimy");
	if (att)
		quatosData.objectsDim(context->n, 1) = atof(att);
	att = quatosToolFindAttr(atts, "dimz");
	if (att)
		quatosData.objectsDim(context->n, 2) = atof(att);

	att = quatosToolFindAttr(atts, "offsetx");
	if (att)
		quatosData.objectsOffset(context->n, 0) = atof(att);
	att = quatosToolFindAttr(atts, "offsety");
	if (att)
		quatosData.objectsOffset(context->n, 1) = atof(att);
	att = quatosToolFindAttr(atts, "offsetz");
	if (att)
		quatosData.objectsOffset(context->n, 2) = atof(att);
}

void parsePort(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;
	
	att = quatosToolFindAttr(atts, "rotation");
	if (!att) {
		fprintf(stderr, "quatosTool: craft '%s' missing rotation attribute\n", quatosData.craftId);
	}
	else {
		quatosData.propDir(0, context->n) = atoi(att);
	}
}

void parseCraft(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;

	att = quatosToolFindAttr(atts, "id");
	if (att && (!*quatosData.craftId || !strcmp(att, quatosData.craftId))) {
		strncpy(quatosData.craftId, att, strlen(att));

		att = quatosToolFindAttr(atts, "config");
		if (!att) {
			fprintf(stderr, "quatosTool: craft '%s' missing config type\n", quatosData.craftId);
		}
		else {
			quatosData.craftType = quatosToolConfigTypeByName(att);
			if (quatosData.craftType < 0) {
				fprintf(stderr, "quatosTool: craft '%s' invalid config type '%s'\n", quatosData.craftId, att);
			}
			else {
				if (quatosData.craftType == CONFIG_CUSTOM) {
					att = quatosToolFindAttr(atts, "motors");
					if (!att || !atoi(att)) {
						fprintf(stderr, "quatosTool: craft '%s' custom type has missing/incorrect motors attribute\n", quatosData.craftId);
						exit(1);
					}
					quatosData.n = atoi(att);
				}
				att = quatosToolFindAttr(atts, "configId");
				if (att)
					quatosData.configId = atoi(att);
				else
					quatosData.configId = configIds[quatosData.craftType];
				context->validCraft = 1;
				resetCraft(context);
			}
		}
	}
}

void parseGeometryMotor(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;

	att = quatosToolFindAttr(atts, "rotation");
	if (!att) {
		fprintf(stderr, "quatosTool: craft '%s' missing geometry->motor rotation attribute\n", quatosData.craftId);
	}
	else {
		quatosData.propDir(0, context->n) = atoi(att);

		att = quatosToolFindAttr(atts, "port");
		if (!att || !atoi(att))
			fprintf(stderr, "quatosTool: craft '%s' has missing/incorrect geometry->motor port attribute\n", quatosData.craftId);
		else
			quatosData.ports(0, context->n) = atoi(att);
	}
}

void XMLCALL startElement(void *ctx, const XML_Char *name, const XML_Char **atts ) {
	parseContext_t *context = (parseContext_t *)ctx;
	int elementId;

	elementId = quatosToolElementIdByName(name);

	if (elementId >= 0 && context->level < (MAX_DEPTH-1)) {
		context->elementIds[++context->level] = elementId;
//		printf("%d: %s\n", context->level, name);

		switch (context->elementIds[context->level]) {
			case ELEMENT_QUATOS_CONFIGURATION:
				context->validCraft = 0;
				break;
			case ELEMENT_CRAFT:
				parseCraft(context, atts);
				break;
			case ELEMENT_PORTS:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_PORT:
				if (context->validCraft)
					parsePort(context, atts);
				break;
			case ELEMENT_GEOMETRY:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_DISTANCE:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_MASS:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_MOTOR:
				if (context->validCraft && context->elementIds[context->level-1] == ELEMENT_GEOMETRY) {
					parseGeometryMotor(context, atts);
				}
				break;
			case ELEMENT_ARM:
				break;
			case ELEMENT_ESC:
				break;
			case ELEMENT_CUBE:
				if (context->validCraft)
					parseCube(context, atts);
				break;
		}
//		while (*atts)
//			printf("\tatt: %s\n", *atts++);
	}
	memset(context->value, 0, sizeof(context->value));
	context->valueLen = 0;
}

void XMLCALL parseChar(void *ctx, const XML_Char *str, int n) {
	parseContext_t *context = (parseContext_t *)ctx;

	context->value[context->valueLen++] = *str;
}

void XMLCALL endElement(void *ctx, const XML_Char *name __attribute__((__unused__)) ) {
	parseContext_t *context = (parseContext_t *)ctx;

	switch (context->elementIds[context->level]) {
		case ELEMENT_QUATOS_CONFIGURATION:
			break;
		case ELEMENT_CRAFT:
			context->validCraft = 0;
			break;
		case ELEMENT_PORTS:
			break;
		case ELEMENT_PORT:
			if (context->validCraft) {
				quatosData.ports(0, context->n) = atoi(context->value);
				context->n++;
			}
			break;
		case ELEMENT_GEOMETRY:
			break;
		case ELEMENT_DISTANCE:
			break;
		case ELEMENT_MASS:
			break;
		case ELEMENT_MOTOR:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					quatosData.massMot = atof(context->value);
				else if (context->elementIds[context->level-1] == ELEMENT_DISTANCE)
					quatosData.distMot = atof(context->value);
				else if (context->elementIds[context->level-1] == ELEMENT_GEOMETRY) {
					quatosData.frameX(context->n) = atof(std::strtok(context->value, ","));
					quatosData.frameY(context->n) = atof(std::strtok(NULL, ","));
					context->n++;
				}
			}
			break;
		case ELEMENT_ARM:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					quatosData.massArm = atof(context->value);
			}
			break;
		case ELEMENT_ESC:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					quatosData.massEsc = atof(context->value);
				else if (context->elementIds[context->level-1] == ELEMENT_DISTANCE)
					quatosData.distEsc = atof(context->value);
			}
			break;
		case ELEMENT_CUBE:
			if (context->validCraft) {
				quatosData.massObjects(context->n) = atof(context->value);
				context->n++;
			}
			break;
	}

	context->level--;
}

void quatosToolUsage(void) {
	fprintf(stderr, "\nUsage:\n");
	fprintf(stderr, "quatosTool [-h | --help] [-d | --debug] [-v | --version] [-c | --craft-id <craft_id>]\n");
	fprintf(stderr, "           [-p | --pid]  [-m | --mix]   [-o | --output <output_file>]\n");
	fprintf(stderr, "           <xml_file>\n\n");
	fprintf(stderr, "   Default usage produces C-style #define code for inclusion or loading directly to AQ.\n");
	fprintf(stderr, "   Using -m (.mix file) produces an INI-format file for use with QGC motor mix configurator.\n");
	fprintf(stderr, "   Using -o without an argument will create an output file named <craft_id>.\n\n");
}

unsigned int quatosToolOptions(int argc, char **argv) {
	int ch;
	char fname[256]; // output file name

	/* options descriptor */
	static struct option longopts[] = {
			{ "help",		no_argument,		NULL,	'h' },
			{ "craft-id",	required_argument,	NULL,	'c' },
			{ "pid",		no_argument,		NULL,	'p' },
			{ "output",		optional_argument,	NULL,	'o' },
			{ "mix",		no_argument,		NULL,	'm' },
			{ "debug",		no_argument,		NULL,	'd' },
			{ "version",	no_argument,		NULL,	'v' },
			{ NULL,			0,					NULL,	0 }
	};

	outFP = stdout;
	outputDebug = 0;

	while ((ch = getopt_long(argc, argv, "hpmdvo::c:", longopts, NULL)) != -1)
		switch (ch) {
		case 'h':
			quatosToolUsage();
			exit(0);
			break;
		case 'p':
			outputPID = 1;
			break;
		case 'm':
			outputMIXfile = 1;
			//outputPID = 1;
			break;
		case 'o':
			if (optarg == NULL && quatosData.craftId) {
				strncpy(fname, quatosData.craftId, sizeof(fname));
				if (outputMIXfile)
					strcat(fname, ".mix");
				else
					strcat(fname, ".param");
			} else if (optarg != NULL)
				strcpy(fname, optarg);
			else {
				fprintf(stderr, "quatosTool: cannot determine output file name\n");
				exit(0);
			}

			outFP = fopen(fname, "w");
			if (outFP == NULL) {
				fprintf(stderr, "quatosTool: cannot open output file '%s'\n", optarg);
				exit(0);
			}
			break;
		case 'c':
			strncpy(quatosData.craftId, optarg, sizeof(quatosData.craftId));
			break;
		case 'd':
			outputDebug = 1;
			break;
		case 'v':
			printf("%s\n", QUATOSTOOL_VERSION);
			exit(0);
		default:
			quatosToolUsage();
			return 0;
		}

	return 1;
}

int quatosToolReadXML(FILE *fp) {
	XML_Parser parser;
	parseContext_t context;
	char c;

	if (!(parser = XML_ParserCreate(NULL))) {
		fprintf(stderr, "quatosTool: cannot create XML parser, aborting\n");
		return -1;
	}

	memset(&context, 0, sizeof(parseContext_t));
	XML_SetUserData(parser, &context);

	XML_SetStartElementHandler(parser, &startElement);
	XML_SetDefaultHandler(parser, &parseChar);
	XML_SetEndElementHandler(parser, &endElement);

	while ((c = fgetc(fp)) != EOF)
		parse(parser, c, 0);

	parse(parser, c, 1);

	XML_ParserFree(parser);

	return 0;
}

typedef struct {
	double mass;
	double x, y, z;
	double dimX, dimY, dimZ;
} object_t;

void quatosToolJCalc(Matrix3d &J, double mass, double x, double y, double z) {
	Matrix3d S;

	S <<	0,	-z,	y,
		z,	0,	-x,
		-y,	x,	0;


	J = J - mass*(S*S);
}

// only cuboid so far
void quatosToolShapeCalc(Matrix3d &J, object_t *obj) {
	double sign[3];
	double mass;
	int x, y, z;
	int i, j, k;

	// divide dimentions into mm cubes
	x = obj->dimX * 1000;
	y = obj->dimY * 1000;
	z = obj->dimZ * 1000;

	sign[0] = (obj->x < 0.0) ? -1.0 : +1.0;
	sign[1] = (obj->y < 0.0) ? -1.0 : +1.0;
	sign[2] = (obj->z < 0.0) ? -1.0 : +1.0;

	mass = obj->mass / (double)(x * y * z);
	//printf("(%d, %d, %d) total mass %f point-mass used: %e\n", x, y, z, obj->mass, mass);

	for (i = 0; i < x; i++)
		for (j = 0; j < y; j++)
			for (k = 0; k < z; k++)
				quatosToolJCalc(J, mass,
					obj->x - quatosData.offsetCG(0) - (obj->dimX/2.0 + (double)i/1000.0) * sign[0],
					obj->y - quatosData.offsetCG(1) - (obj->dimY/2.0 + (double)j/1000.0) * sign[1],
					obj->z - quatosData.offsetCG(2) - (obj->dimZ/2.0 + (double)k/1000.0) * sign[2]);
}

void quatosToolObjCalc() {
	object_t objs[256];
	int o;
	int i;

	memset(objs, 0, sizeof(objs));

	o = 0;
	for (i = 0; i < quatosData.n; i++) {
		// Motor
		objs[o].mass = quatosData.massMot;
		objs[o].x = quatosData.frameX(i);
		objs[o].y = quatosData.frameY(i);
		if (quatosData.craftType != CONFIG_CUSTOM) {
			objs[o].x *= quatosData.distMot;
			objs[o].y *= quatosData.distMot;
		}
		objs[o].z = 0.0;
		o++;

		// ESC
		objs[o].mass = quatosData.massEsc;
		if (quatosData.craftType != CONFIG_CUSTOM) {
			objs[o].x = quatosData.frameX(i) * quatosData.distEsc;
			objs[o].y = quatosData.frameY(i) * quatosData.distEsc;
			objs[o].z = 0.0;
		}
		else {
			double norm = sqrt(quatosData.frameX(i)*quatosData.frameX(i) + quatosData.frameY(i)*quatosData.frameY(i));

			objs[o].x = quatosData.frameX(i) / norm * quatosData.distEsc;
			objs[o].y = quatosData.frameY(i) / norm * quatosData.distEsc;
			objs[o].z = 0.0;
		}
		o++;

		// ARM
		objs[o].mass = quatosData.massArm;
		objs[o].x = quatosData.frameX(i) / 2.0;
		objs[o].y = quatosData.frameY(i) / 2.0;
		if (quatosData.craftType != CONFIG_CUSTOM) {
			objs[o].x *= quatosData.distMot;
			objs[o].y *= quatosData.distMot;
		}
		objs[o].z = 0.0;
		o++;
	}

	for (i = 0; i < quatosData.massObjects.size(); i++) {
		objs[o].mass = quatosData.massObjects(i);
		objs[o].x = quatosData.objectsOffset(i, 0);
		objs[o].y = quatosData.objectsOffset(i, 1);
		objs[o].z = quatosData.objectsOffset(i, 2);

		objs[o].dimX = quatosData.objectsDim(i, 0);
		objs[o].dimY = quatosData.objectsDim(i, 1);
		objs[o].dimZ = quatosData.objectsDim(i, 2);
		o++;
	}

	quatosData.totalMass = 0.0;
	quatosData.offsetCG.setZero();
	for (i = 0; i < o; i++) {
		objs[i].mass /= 1000;			// g => Kg
		quatosData.offsetCG(0) += objs[i].mass * objs[i].x;
		quatosData.offsetCG(1) += objs[i].mass * objs[i].y;
		quatosData.offsetCG(2) += objs[i].mass * objs[i].z;

		quatosData.totalMass += objs[i].mass;
	}
	quatosData.offsetCG /= quatosData.totalMass;
	quatosData.objectsCount = o;

	// calculate J matrix
	quatosData.J.setZero();
	for (i = 0; i < o; i++) {
		if (objs[i].dimX != 0.0 && objs[i].dimY != 0.0 && objs[i].dimZ != 0.0) {
			//  dimensioned shapes
			quatosToolShapeCalc(quatosData.J, &objs[i]);
		}
		else {
			// point masses
			quatosToolJCalc(quatosData.J, objs[i].mass, objs[i].x - quatosData.offsetCG(0), objs[i].y - quatosData.offsetCG(1), objs[i].z - quatosData.offsetCG(2));
		}
	}
}

void quatosToolCalc(void) {
//	VectorXd frameX, frameY;
	MatrixXd A;
	MatrixXd B;

	quatosData.motorX.resize(1, quatosData.n);
	quatosData.motorY.resize(1, quatosData.n);

	//frameX.resize(quatosData.n);
	//frameY.resize(quatosData.n);

	// calculate x/y coordinates for each motor
	switch (quatosData.craftType) {
		case CONFIG_QUAD_PLUS:
			quatosData.frameX << 1.0, 0.0, -1.0, 0.0;
			quatosData.frameY << 0.0, 1.0, 0.0, -1.0;
			break;
		case CONFIG_QUAD_X:
			quatosData.frameX << sqrt(2.0)/2.0, sqrt(2.0)/2.0, -sqrt(2.0)/2.0, -sqrt(2.0)/2.0;
			quatosData.frameY << -sqrt(2.0)/2.0, sqrt(2.0)/2.0, sqrt(2.0)/2.0, -sqrt(2.0)/2.0;
			break;
		case CONFIG_HEX_PLUS:
			quatosData.frameX << 0.0,	sqrt(3.0)/2.0,	sqrt(3.0)/2.0,	0.0,	-sqrt(3.0)/2.0,	-sqrt(3.0)/2.0;
			quatosData.frameY << 1.0,	0.5,		-0.5,		-1.0,	-0.5,		0.5;
			quatosData.frameX << 1.0, 0.5, -0.5, -1, -0.5, 0.5;
			quatosData.frameY << 0.0, sqrt(3)/2.0, sqrt(3.0)/2.0, 0.0, -sqrt(3.0)/2.0, -sqrt(3.0)/2.0;
			break;
		case CONFIG_HEX_X:
			quatosData.frameX << sqrt(3.0)/2.0,	sqrt(3.0)/2.0, 0.0, -sqrt(3.0)/2.0, -sqrt(3.0)/2.0,	0.0;
			quatosData.frameY <<  -0.5,	0.5, 1.0, 0.5, -0.5, -1.0;
			break;
		case CONFIG_OCTO_PLUS:
			quatosData.frameY << 0,	cosf(315 *DEG_TO_RAD),	1,	cosf(45 *DEG_TO_RAD),	0,	cosf(135 *DEG_TO_RAD),	-1,	cosf(225 *DEG_TO_RAD);
			quatosData.frameX << 1,	cosf(45 *DEG_TO_RAD),	0,	cosf(135 *DEG_TO_RAD),	-1,	cosf(225 *DEG_TO_RAD),	0,	cosf(315 *DEG_TO_RAD);
			break;
		case CONFIG_OCTO_X:
			quatosData.frameY << cosf(247.5 *DEG_TO_RAD),	cosf(292.5 *DEG_TO_RAD),	cosf(337.5 *DEG_TO_RAD),	cosf(22.5 *DEG_TO_RAD),
					     cosf(67.5 *DEG_TO_RAD),	cosf(112.5 *DEG_TO_RAD),	cosf(157.5 *DEG_TO_RAD),	cosf(202.5 *DEG_TO_RAD);
			quatosData.frameX << cosf(337.5 *DEG_TO_RAD),	cosf(22.5 *DEG_TO_RAD),		cosf(67.5 *DEG_TO_RAD),		cosf(112.5 *DEG_TO_RAD),
					     cosf(157.5 *DEG_TO_RAD),	cosf(202.5 *DEG_TO_RAD),	cosf(247.5 *DEG_TO_RAD),	cosf(292.5 *DEG_TO_RAD);
			break;
	}

	// calc GG offset & J matrix
	quatosToolObjCalc();

	quatosData.motorX = quatosData.frameX.transpose();
	quatosData.motorY = quatosData.frameY.transpose();

	if (quatosData.craftType != CONFIG_CUSTOM) {
		quatosData.motorX *= quatosData.distMot;
		quatosData.motorY *= quatosData.distMot;
	}
/*
std::cout << "quatosData.motorX: " << quatosData.motorX << std::endl;
std::cout << "quatosData.motorY: " << quatosData.motorY << std::endl;
*/

	// adjust for CG offset
	quatosData.motorX -= VectorXd::Ones(quatosData.n) * quatosData.offsetCG(0);
	quatosData.motorY -= VectorXd::Ones(quatosData.n) * quatosData.offsetCG(1);

	quatosData.propDir *= -1.0;								// our sense of rotation is counter intuitive

	A.resize(3, quatosData.n);
	B.resize(3, 1);

	// Roll
	A <<	quatosData.motorX,
		MatrixXd::Ones(1, quatosData.n),
		-quatosData.motorY;
	B <<	0,
		0,
		1;

	pseudoInverse(A, quatosData.ROLL);
	quatosData.ROLL *= B;

	// Pitch
	A <<	-quatosData.motorY,
		MatrixXd::Ones(1, quatosData.n),
		quatosData.motorX;
	B <<	0,
		0,
		1;

	pseudoInverse(A, quatosData.PITCH);
	quatosData.PITCH *= B;

	// Yaw
	A <<	quatosData.motorX,
		quatosData.motorY,
		quatosData.propDir;
	B <<	0,
		0,
		1;

	pseudoInverse(A, quatosData.YAW);
	quatosData.YAW *= B;

	// Throttle
	A.resize(4, quatosData.n);
	B.resize(4, 1);

	A <<	quatosData.motorX,
		quatosData.motorY,
		quatosData.propDir,
		MatrixXd::Ones(1, quatosData.n);
	B <<	0,
		0,
		0,
		quatosData.n;

	pseudoInverse(A, quatosData.THROT);
	quatosData.THROT *= B;


	// PD
	quatosData.PD.resize(quatosData.n, 3);
	quatosData.PD <<	quatosData.ROLL,
			quatosData.PITCH,
			quatosData.YAW,

	// M
	quatosData.M.resize(3, quatosData.n);
	quatosData.M <<	-quatosData.motorY,
			quatosData.motorX,
			quatosData.propDir;

	// Mt
	quatosData.Mt.resize(quatosData.n, 4);
	quatosData.Mt << quatosData.THROT, quatosData.PD * (quatosData.M*quatosData.PD).inverse();

	// PID
	quatosData.PID.setZero(4, quatosData.n);
	quatosData.PID <<	quatosData.Mt.col(0).transpose() / quatosData.Mt.col(0).cwiseAbs().maxCoeff(),
			quatosData.Mt.col(1).transpose() / quatosData.Mt.col(1).cwiseAbs().maxCoeff(),
			quatosData.Mt.col(2).transpose() / quatosData.Mt.col(2).cwiseAbs().maxCoeff(),
			quatosData.Mt.col(3).transpose() / quatosData.Mt.col(3).cwiseAbs().maxCoeff();
	quatosData.PID = quatosData.PID.transpose().eval() * 100.0;

}

template<typename _Matrix_Type_>
void quatosToolMatrixOutput(const char *mtrxName, _Matrix_Type_ &mtrx) {
	int i, ii, j, maxIdx;
	float val, t, p, r, y;

	if (!outputMIXfile)
		displayMatrix(mtrxName, mtrx);

	// output .mix file for QGC (.ini file format)
	if (outputMIXfile) {
		if (!strcmp(mtrxName, "J")) {
			fprintf(outFP, "[QUATOS]\n");
			fprintf(outFP, "J_ROLL=%g\n", mtrx(0, 0));
			fprintf(outFP, "J_PITCH=%g\n", mtrx(1, 1));
			fprintf(outFP, "J_YAW=%g\n", mtrx(2, 2));
			fprintf(outFP, "\n");
			return;
		}
		maxIdx = strcmp(mtrxName, "M") ? 4 : 3;  // "M" matrix has only 3 members
		fprintf(outFP, "\n"); // blank line after "info" section
		for (ii=0; ii < maxIdx; ii++) {  // loop over each control direction (T,P,R,Y (Mt or PID) or R,P,Y (M))
			switch (ii) {
				case 0 :
					fprintf(outFP, "[%s]\n", maxIdx == 4 ? "Throttle" : "MM_Roll");
					break;
				case 1 :
					fprintf(outFP, "[%s]\n", maxIdx == 4 ? "Roll" : "MM_Pitch");
					break;
				case 2 :
					fprintf(outFP, "[%s]\n", maxIdx == 4 ? "Pitch" : "MM_Yaw");
					break;
				case 3 :
					fprintf(outFP, "[Yaw]\n");
					break;
			}
			for (i = 1; i <= NUM_PORTS; i++) {
				val = 0.0f;
				j = quatosToolFindPort(i);
				if (j >= 0) {
					if (maxIdx == 4) // "M" or "PID"
						val = mtrx(j, ii);
					else
						val = mtrx(ii, j);
				}
				fprintf(outFP, "Motor%d=%g\n", i, round(val * 10000)/10000); // %g prints no trailing decimals when they're zero, unlike %f
			}
			fprintf(outFP, "\n"); // blank line ends section
		}
	}
	// output generated matrix and #defines for DEFAULT_MOT_PWRD params
	else {
		if (!strcmp(mtrxName, "J")) {
			fprintf(outFP, "#define DEFAULT_QUATOS_J_ROLL\t%g\n", mtrx(0, 0));
			fprintf(outFP, "#define DEFAULT_QUATOS_J_PITCH\t%g\n", mtrx(1, 1));
			fprintf(outFP, "#define DEFAULT_QUATOS_J_YAW\t%g\n", mtrx(2, 2));
			fprintf(outFP, "\n");
			return;
		}
		for (i = 1; i <= NUM_PORTS; i++) {
			t = 0.0;
			p = 0.0;
			r = 0.0;
			y = 0.0;
			j = quatosToolFindPort(i);

			// "Mt" and "PID" matrixes
			if (strcmp(mtrxName, "M")) {
				if (j >= 0) {
						t = mtrx(j, 0);
						r = mtrx(j, 1);
						p = mtrx(j, 2);
						y = mtrx(j, 3);
				}
				fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_T\t%+f\n", i, t);
				fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_P\t%+f\n", i, p);
				fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_R\t%+f\n", i, r);
				fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_Y\t%+f\n", i, y);
			}
			// "M" matrix
			else {
				if (j >= 0) {
						r = mtrx(0, j);
						p = mtrx(1, j);
						y = mtrx(2, j);
				}
				fprintf(outFP, "#define DEFAULT_QUATOS_MM_P%02d\t%+f\n", i, p);
				fprintf(outFP, "#define DEFAULT_QUATOS_MM_R%02d\t%+f\n", i, r);
				fprintf(outFP, "#define DEFAULT_QUATOS_MM_Y%02d\t%+f\n", i, y);
			}
		}
		fprintf(outFP, "\n");
	}
}

void quatosToolDbgCraftData(void) {
	std::cerr << "quatosData.ports: " << quatosData.ports << std::endl;
	std::cerr << "quatosData.propDir: " << quatosData.propDir << std::endl;
	std::cerr << "quatosData.distMot: " << quatosData.distMot << std::endl;
	std::cerr << "quatosData.distEsc: " << quatosData.distEsc << std::endl;
	std::cerr << "quatosData.massMot: " << quatosData.massMot << std::endl;
	std::cerr << "quatosData.massEsc: " << quatosData.massEsc << std::endl;
	std::cerr << "quatosData.massArm: " << quatosData.massArm << std::endl;
	std::cerr << "quatosData.massObjects: " << quatosData.massObjects << std::endl;
	std::cerr << "quatosData.objectsDim: " << quatosData.objectsDim << std::endl;
	std::cerr << "quatosData.objectsOffset: " << quatosData.objectsOffset << std::endl << std::endl;
}

int main(int argc, char **argv) {
	FILE *fp;
	int i;
	unsigned long portOrder = 0;

	memset(&quatosData, 0, sizeof(quatosData));

	if (!quatosToolOptions(argc, argv)) {
		fprintf(stderr, "Init failed, aborting\n");
		return 0;
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "quatosTool: requires xml file argument, aborting\n");
		return -1;
	}
	if (!(fp = fopen(argv[0], "r"))) {
		fprintf(stderr, "quatosTool: cannot open XML file '%s', aborting\n", argv[1]);
		return -1;
	}

	if (quatosToolReadXML(fp) < 0)
		return -1;

	if (!quatosData.validCraft) {
		fprintf(stderr, "quatosTool: craft is not valid, aborting\n");
		return -1;
	}

	if (outputDebug)
		quatosToolDbgCraftData();

	quatosToolCalc();

	if (outputMIXfile) {
		fprintf(outFP, "[META]\n");
		fprintf(outFP, "ConfigId=%d\n", quatosData.configId);
		// output port ordering hint for GCS GUI
		fprintf(outFP, "PortOrder=");
		for (int i=0; i < quatosData.ports.size(); ++i)
			fprintf(outFP, "%d,", (int)quatosData.ports(i));
		fprintf(outFP, "\n");
	}
	fprintf(outFP, "Tool_Version=%s\n", QUATOSTOOL_VERSION);
	fprintf(outFP, "Craft=%s\n", quatosData.craftId);
	fprintf(outFP, "Motors=%d\n", quatosData.n);
	fprintf(outFP, "Mass=%f Kg (%d objects)\n", quatosData.totalMass, quatosData.objectsCount);
	fprintf(outFP, "CG_Offset=%f, %f, %f\n", quatosData.offsetCG(0), quatosData.offsetCG(1), quatosData.offsetCG(2));

	if (outputPID) {
		quatosToolMatrixOutput("PID", quatosData.PID);
	} else {
		quatosToolMatrixOutput("Mt", quatosData.Mt);
		quatosToolMatrixOutput("M", quatosData.M);
		quatosToolMatrixOutput("J", quatosData.J);
	}

	// output port ordering hint for GCS GUI
	if (!outputMIXfile) {
		// save port order (bit mask: first 8 bits are confgId, next 4 bits is first port used, next 4 is 2nd port used, etc., up to 32 bits (6 ports plus id))
		i = std::min((int)5, (int)quatosData.ports.size()-1);
		for (; i >= 0; --i)
			portOrder |= static_cast<unsigned long>(quatosData.ports(i)) << (8 + (4 * i)); // save 8 bits for configId

		portOrder |= quatosData.configId;
		float val = *(float *) &portOrder;
		fprintf(outFP, "#define DEFAULT_MOT_FRAME\t%.20g\n", val);

		// if more than 6 motors, need 2nd bitmask to store the other motor positions
		// 2nd port order (bit mask: first 4 bits is 7th port used, next 4 is 8th port used, etc., up to 32 bits (8 ports))
		if (quatosData.ports.size() > 6) {
			portOrder = 0;
			i = std::min((int)7, (int)quatosData.ports.size()-7);
			for (; i >= 0; --i)
				portOrder |= static_cast<unsigned long>(quatosData.ports(i+6)) << (4 * i);

			val = *(float *) &portOrder;
			fprintf(outFP, "#define DEFAULT_MOT_FRAME_H\t%.20g\n", val);
		}
	}

	return 0;
}
