// example1.c 
//

#include "stdio.h"
#include "vscphelperlib.h"

//#define HOST "demo.vscp.org:9598"
#define HOST "127.0.0.1:9598"

int main(int argc, char* argv[])
{
    long handle;
    unsigned long rv;
    unsigned char v1,v2,v3;
    
    // New session
    printf("Starting a new session to [" HOST "] ...");
    handle = vscphlp_newSession();
    if (handle) printf("OK\n");

    //vscphlp_setAfterCommandSleep( handle, 200 );

    printf("Open channel...\n");
    if ( VSCP_ERROR_SUCCESS == vscphlp_open( handle, 
                 				HOST,
                 				"admin",
    	             			        "secret" ) ) {
		
		printf("\a vscphlp_open: Success!\n");
	
		if ( VSCP_ERROR_SUCCESS == vscphlp_noop( handle ) ) {
		printf("vscphlp_noop 1: Success!\n");
		}   
		else {
			printf("\a * * * * * *  Failed noop! * * * * * *  v\n");
		}
    
    
		if ( VSCP_ERROR_SUCCESS == (rv = vscphlp_getVersion( handle, &v1, &v2, &v3 ) ) ) {
			printf( "----------------------------------------------------------------\n" );
			printf( "Command success: vscphlp_getVersion\n" );
			printf( "Version for VSCP daemon is %d.%d.%d\n", v1,v2,v3 );
			printf( "----------------------------------------------------------------\n" );
		}
		else {
			printf( "----------------------------------------------------------------\n" );
			printf( "\agetversion failed %ld\n", rv );
			printf( "----------------------------------------------------------------\n" );
		}
    
		if ( VSCP_ERROR_SUCCESS == vscphlp_noop( handle ) ) {
			printf("vscphlp_noop 2: Success!\n");
		}   
		else {
			printf("\a * * * * * *  Failed noop! * * * * * *  v\n");
		}

		if ( VSCP_ERROR_SUCCESS == vscphlp_close( handle ) ) {
			printf("vscphlp_close: Success!\n");
		}
	
		vscphlp_closeSession( handle );
	
	
    } 
    else {
        printf("\a * * * * * *  Failed to open connection!  * * * * * * *\n");
	vscphlp_closeSession( handle ); 
        return -1;
    }

	
   
	return 0;
}
