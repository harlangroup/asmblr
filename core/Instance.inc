<?php
/**
 * @file Instance.inc asmblr instance controller.
 * @author Stackware, LLC
 * @version 4.0
 * @copyright Copyright (c) 2012-2014 Stackware, LLC. All Rights Reserved.
 * @copyright Licensed under the GNU General Public License
 * @copyright See COPYRIGHT.txt and LICENSE.txt.
 */
namespace asm;


/**
 * Base application loader and request router.
 *
 * It should be extended and customized in index.php.
 *
 * @note We're using only Google Drive (file system) and no mongo/etc.
 *
 * @todo Include hook for instance-wide includes/
 * @todo Consider an instance manifest.
 */
abstract class Instance
{
    protected $Apps = array();

    protected $CacheDir = '';

    protected $CacheManifest = FALSE;
    protected $CacheApp = FALSE;

    // these are typically detected correctly but can be overriden.
    public $APP_ROOT = '';
    public $DOC_ROOT = '';


    public function __construct()
    {
    	if( empty($this->DOC_ROOT) )
    		$this->DOC_ROOT = getcwd().'/';

    	if( empty($this->APP_ROOT) )
    		$this->APP_ROOT = $this->DOC_ROOT.'../APP_ROOT/';
    }

    /**
     * Match a requested hostname to an app.
     *
     * The hostname is resolved to an app manifest which then
     * loads the app itself into memory and returns the manifest.
     *
     * @param array $Request Request array to process.
     * @retval array Manifest of the routed to app.
     * @throws Exception Can't read directory for requested hostname.
     * @throws Exception Unknown requested hostname.
     * @todo Implement locking/flock() for writing cached files.
     */
    public function Match( $Request )
    {
    	$Hostname = Hostname::ToString($Request['Hostname']);

    	// determine if we have an app for the exact hostname
    	if( isset($this->Apps[$Hostname]) )
    	{
    	    $AppForThat = $this->Apps[$Hostname];
    	}
    	// if not, find the most general match which must be defined with a leading period
    	else
        {
            foreach( Hostname::Order($Request['Hostname']) as $H )
            {
                if( isset($this->Apps[$H]) )
                {
                    $AppForThat = $this->Apps[$H];
                    break;
                }
            }
        }

        if( empty($AppForThat) )
            throw new Exception("Unknown requested hostname '{$Hostname}'");

        // make sure we're not misconfigured
        if( !is_dir("{$this->APP_ROOT}/{$AppForThat}") )
            throw new Exception("Can't read {$this->APP_ROOT}/{$AppForThat} for requested hostname '{$Hostname}'");

        if( $this->CacheManifest === TRUE )
        {
            if( (@include "{$this->CacheDir}/{$AppForThat}.manifest.inc") === FALSE )
            {
                $Manifest = $this->BuildAppManifest("{$this->APP_ROOT}/{$AppForThat}");
                file_put_contents("{$this->CacheDir}/{$AppForThat}.manifest.inc",'<?php $Manifest = '.var_export($Manifest,TRUE).';');
            }
        }
        else
        {
            // clear an existing cache
            @unlink("{$this->CacheDir}/{$AppForThat}.manifest.inc");
            $Manifest = $this->BuildAppManifest("{$this->APP_ROOT}/{$AppForThat}");
        }

        if( $this->CacheApp === TRUE )
        {
            // PHP bug: always throws fatal error with no ability to detect @
            if( (@include "{$this->CacheDir}/{$AppForThat}.app.inc") === FALSE )
            {
                file_put_contents("{$this->CacheDir}/{$AppForThat}.app.inc",$this->BuildApp($Manifest));
                include "{$this->CacheDir}/{$AppForThat}.app.inc";
            }
        }
        else
        {
            // clear an existing cache
            @unlink("{$this->CacheDir}/{$AppForThat}.app.inc");
            $this->LoadApp($Manifest);
        }

        return $Manifest;
    }


    // use a manifest.inc if it exists otherwise build from manifest.gsheet in the $AppPath
    // $AppPath is trusted
    protected function BuildAppManifest( $AppPath )
    {
        $SP = Path::Init($AppPath);
        $SP['IsDir'] = TRUE;
        $SP = Path::ToString($SP);

        // load and process as a Google Sheet and return a manifest array
        if( !is_readable($SP.'manifest.gsheet') )
            throw new Exception("Can't find manifest.inc or manifest.gsheet in $AppPath");

        $Manifest = array('AppRoot'=>$SP,'Config'=>array(),'Directives'=>array(),'Pages'=>array(),'PathPageMap'=>array(),'Templates'=>array());

        $SS = file_get_contents($SP.'manifest.gsheet');
        $SS = json_decode($SS,TRUE);
        $SS = URL::Init($SS['url']);
        $SS['Path']['Segments'][1] = 'pub';
        $SS['Query']['single'] = 'true';
        $SS['Query']['output'] = 'csv';
        $SS['Query']['usp'] = NULL;

        $ConfigDone = $PagesDone = $TemplatesDone = FALSE;
        for( $i = 0; $i < 10; ++$i )
        {
            $SS['Query']['gid'] = $i;
            $B = file(URL::ToString($SS));

            if( empty($B[0]) )
                continue;

            // config tab - Key, Value
            if( strpos($B[0],'Key,Value') !== FALSE )
            {
                array_shift($B);
                foreach( $B as $L )
                {
                    $L = str_getcsv($L);
                    if( empty($L[0]) )
                        continue;
                    else if( $L[0][0] === '$' )
                        $Manifest['Directives'][] = array(str_replace('$','',$L[0]),$L[1],$L[2]);
                    else if( $L[0] === 'AppFunction' )
                        $Manifest['Config']['AppFunction'] = $this->ParseFunctionName($L[1]);
                    else
                        $Manifest['Config'][$L[0]] = $L[1];
                }

                if( empty($Manifest['Config']['Hostname']) )
                {
                    trigger_error("Skipping invalid config at $V - No Hostname");
                    break;
                }

                $ConfigDone = TRUE;
            }
            // pages tab - Name, Path, Status, Function, Directives
            else if( strpos($B[0],'Name,Path,Status') !== FALSE )
            {
                array_shift($B);
                $LastPage = '';
                foreach( $B as $L )
                {
                    $L = str_getcsv($L);
                    if( !empty($L[0]) )
                    {
                        $P = Page::Init($L[0],$L[1],$L[2],$this->ParseFunctionName($L[3]));
                        $Manifest['Pages'][$P['Name']] = $P;
                        $Manifest['PathPageMap'][$P['Path']] = $P['Name'];
                        $LastPage = $P['Name'];
                    }

                    if( !empty($L[4]) )
                    {
                        if( !empty($L[5]) )
                            $D = array($L[4],$L[5],$L[6]);
                        else
                            $D = explode(',',$L[4]);

                        if( !empty($LastPage) )
                            $Manifest['Pages'][$LastPage]['Directives'][] = $D;
                    }
                }

                $PagesDone = TRUE;
            }
            // templates tab - Name, Function - we just store the name/function mapping and the rest is init in the AppFile
            // name must contain a prefix if the template file is in a sub-directory
            else if( strpos($B[0],'Name,Function') !== FALSE )
            {
                array_shift($B);
                foreach( $B as $L )
                {
                    $L = str_getcsv($L);
                    if( empty($L[0]) )
                        continue;
                    else
                        $Manifest['Templates'][$L[0]] = $this->ParseFunctionName($L[1]);
                }

                $TemplatesDone = TRUE;
            }

            if( $ConfigDone && $PagesDone && $TemplatesDone )
                break;
        }

        // lastly, add some instance info - handy for debugging
        $Manifest['CacheManifest'] = $this->CacheManifest;
        $Manifest['CacheApp'] = $this->CacheApp;

        return $Manifest;
    }

    // include() each file from functions/lib when in non-cache/dev mode
    // templates aren't included themselves, though the name of each file with any prefix is saved in the manifest
    protected function LoadApp( &$Manifest )
    {
        $AppRoot = $Manifest['AppRoot'];

        if( !is_dir($AppRoot) || !is_readable($AppRoot) )
            throw new Exception("Unreadable AppRoot '{$AppRoot}'");

        $Hostname = $Manifest['Config']['Hostname'];

        // PHP Bug - can't use FilesystemIterator::CURRENT_AS_PATHNAME |  FilesystemIterator::KEY_AS_FILENAME with recursive dirs
        $Flags = \FilesystemIterator::SKIP_DOTS | \FilesystemIterator::UNIX_PATHS;

        // subdirectories are NOT prefixed and each file just include()'d
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}lib",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        foreach( $fs as $K => $V )
        {
            include $K;
        }

        // subdirectories are NOT prefixed and each file just include()'d
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}functions",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        foreach( $fs as $K => $K )
        {
            include $K;
        }


        // subdirectories ARE prefixed and a Template struct is created with the proper name and filesystem path and merged with
        // any Function definition from the manifest - each is stored in the manifest - the actual template is NOT include()'d
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}templates",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        foreach( $fs as $K => $V )
        {
            $P = Path::Init($K);
            $P = Path::Bottom($P,2);

            if( $P['Segments'][0] !== 'templates' )
                $Prefix = $P['Segments'][0].'_';
            else
                $Prefix = '';

            $Buf = file_get_contents($K);

            if( strpos(substr($Buf,0),"\n@@@") === FALSE )
            {
                $P['Segments'][1] = pathinfo($P['Segments'][1],PATHINFO_FILENAME);

                if( !empty($Manifest['Templates'][$Prefix.$P['Segments'][1]]) )
                    $F = $Manifest['Templates'][$Prefix.$P['Segments'][1]];
                else
                    $F = array();

                $Manifest['Templates'][$Prefix.$P['Segments'][1]] = Template::Init($Prefix.$P['Segments'][1],$K,$F);
            }
            // supporting @@@ is a bit annoying but very useful for SQL so we're keeping it for now
            // note that these DO get their body populated
            else
            {
                $B = preg_split("/\s*@@@(\w+[a-zA-Z0-9\-]*)/m",$Buf,0,PREG_SPLIT_DELIM_CAPTURE|PREG_SPLIT_NO_EMPTY);
                $CNT = count($B);
                for( $i = 0; $i < $CNT; $i+=2 )
                {
                    if( !empty($Manifest['Templates'][$Prefix.$B[$i]]) )
                        $F = $Manifest['Templates'][$Prefix.$B[$i]];
                    else
                        $F = array();

                    $Manifest['Templates'][$Prefix.$B[$i]] = Template::Init($Prefix.$B[$i],$K,$F,$B[$i+1]);
                }
            }
        }
    }

    protected function BuildApp( &$Manifest )
    {
        $AppRoot = $Manifest['AppRoot'];
        if( !is_dir($AppRoot) || !is_readable($AppRoot) )
        	throw new Exception("Unreadable AppRoot '{$AppRoot}'");

        $Hostname = $Manifest['Config']['Hostname'];

        $AppFile = '';

        // PHP Bug - can't use FilesystemIterator::CURRENT_AS_PATHNAME |  FilesystemIterator::KEY_AS_FILENAME with recursive dirs
        $Flags = \FilesystemIterator::SKIP_DOTS | \FilesystemIterator::UNIX_PATHS;

        // subdirectories are NOT prefixed
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}lib",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        foreach( $fs as $K => $V )
        {
            $AppFile .= "\n\n\n/*** {$K} ***/";

            $T = php_strip_whitespace($K);
            if( strpos($T,'<?php') === 0 )
                $AppFile .= "\n".substr($T,5);
            else
                $AppFile .= $T;
        }

        // subdirectories are NOT prefixed
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}functions",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        foreach( $fs as $K => $K )
        {
            $AppFile .= "\n\n\n/*** {$K} ***/";

            $T = php_strip_whitespace($K);
            if( strpos($T,'<?php') === 0 )
                $AppFile .= "\n".substr($T,5);
            else
                $AppFile .= $T;
        }

        // subdirectories ARE prefixed and merge any Function definition from the manifest
        $AppFile .= "\n\n\n/*** {$AppRoot}templates ***/";
        $dir = new \RecursiveDirectoryIterator("{$AppRoot}templates",$Flags);
        $fs = new \RecursiveIteratorIterator($dir);
        $Templates = array();
        foreach( $fs as $K => $V )
        {
            $P = Path::Init($K);
            $P = Path::Bottom($P,2);

            if( $P['Segments'][0] !== 'templates' )
                $Prefix = $P['Segments'][0].'_';
            else
                $Prefix = '';

            $Buf = file_get_contents($K);

            if( strpos(substr($Buf,0),"\n@@@") === FALSE )
            {
                $P['Segments'][1] = pathinfo($P['Segments'][1],PATHINFO_FILENAME);

                if( !empty($Manifest['Templates'][$Prefix.$P['Segments'][1]]) )
                    $F = $Manifest['Templates'][$Prefix.$P['Segments'][1]];
                else
                    $F = array();

                // basically adding Buf is the only difference from LoadApp() - for now :)
                $Manifest['Templates'][$Prefix.$P['Segments'][1]] = Template::Init($Prefix.$P['Segments'][1],$K,$F,$Buf);
            }
            else
            {
                $B = preg_split("/\s*@@@(\w+[a-zA-Z0-9\-]*)/m",$Buf,0,PREG_SPLIT_DELIM_CAPTURE|PREG_SPLIT_NO_EMPTY);
                $CNT = count($B);
                for( $i = 0; $i < $CNT; $i+=2 )
                {
                    if( !empty($Manifest['Templates'][$Prefix.$B[$i]]) )
                        $F = $Manifest['Templates'][$Prefix.$B[$i]];
                    else
                        $F = array();

                    $Manifest['Templates'][$Prefix.$B[$i]] = Template::Init($Prefix.$B[$i],$K,$F,$B[$i+1]);
                }
            }
        }

        return "<?php\n\n{$AppFile} \n\n \$Manifest['Templates'] = ".var_export($Manifest['Templates'],TRUE).';';
    }

    protected function ParseFunctionName( $F )
    {
        if( strpos($F,'::') === FALSE )
        {
            return trim(str_replace(array('(',')'),'',$F));
        }
        else
        {
            $T = explode('::',$F);
            return array(trim($T[0]),trim(str_replace(array('(',')'),'',$T[1])));
        }
    }

    /**
     * Determine whether we're executing in a Windows environment.
     *
     * @retval boolean TRUE if the application is executing in a Windows environment.
     * @todo Should PHP_OS be used instead?
     */
    public static function IsWindows()
    {
        return isset($_SERVER['SystemRoot']);
    }
}